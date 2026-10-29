/**
 * @file index.ts
 * @brief Standard network utility device (std.net): DNS resolution, TCP reachability
 *        probes and URL normalization.
 *
 * @details Ships with the rack as a standard device (docs/design.md §3.1). Every external
 *          effect crosses an injectable port (a DNS resolver and a TCP connector) so unit
 *          tests never touch the network: createWithDeps() wires caller-supplied fakes while
 *          create() wires the Node built-ins (node:dns/promises, node:net and the global
 *          URL). Zero npm dependencies. start() publishes the three methods; stop() aborts
 *          every in-flight probe through a lifetime AbortController — each settles as
 *          {ok: false}, its connection is destroyed immediately and its timeout timer is
 *          cleared — then waits until all probes settled before returning. After stop()
 *          resolves no probe callback can fire any more. All errors are RackErrors and all
 *          args/results are JSON values.
 */

import { resolve4, resolve6 } from 'node:dns/promises';
import { createConnection } from 'node:net';
import { definePlugin, RackError } from '../../sdk/index.ts';
import type {
  InvokeRequest,
  JsonObject,
  JsonValue,
  MethodDesc,
  PluginServices,
  RackPlugin,
} from '../../sdk/index.ts';

// ---------------------------------------------------------------------------
// Identity and published data contract.
// ---------------------------------------------------------------------------

/** @brief Canonical plugin identity announced to the host. */
const NET_PLUG_ID = 'std.net';

/** @brief Business version published by this device. */
const NET_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Numeric method id of the DNS resolution method. */
const DNS_RESOLVE_METHOD_ID = 1;

/** @brief Numeric method id of the TCP probe method. */
const TCP_PROBE_METHOD_ID = 2;

/** @brief Numeric method id of the URL normalization method. */
const URL_NORMALIZE_METHOD_ID = 3;

/** @brief Method names matching the numeric ids above; part of the data-only contract. */
const DNS_RESOLVE_METHOD_NAME = 'dns.resolve';
const TCP_PROBE_METHOD_NAME = 'tcp.probe';
const URL_NORMALIZE_METHOD_NAME = 'url.normalize';

/** @brief Probe timeout used when args.timeoutMs is absent. */
const DEFAULT_PROBE_TIMEOUT_MS = 2000;

/** @brief Inclusive bounds of a valid TCP port. */
const MIN_TCP_PORT = 1;
const MAX_TCP_PORT = 65535;

/** @brief Methods announced through the capability service during start(). */
const NET_METHODS: readonly MethodDesc[] = [
  {
    id: DNS_RESOLVE_METHOD_ID,
    name: DNS_RESOLVE_METHOD_NAME,
    description: 'Resolve A and AAAA records for one host; merged, deduplicated, v4 first.',
    inputSchema: {
      type: 'object',
      properties: { host: { type: 'string' } },
      required: ['host'],
    },
    outputSchema: {
      type: 'object',
      properties: { addresses: { type: 'array', items: { type: 'string' } } },
      required: ['addresses'],
    },
  },
  {
    id: TCP_PROBE_METHOD_ID,
    name: TCP_PROBE_METHOD_NAME,
    description: 'Probe TCP reachability with a hard timeout; every failure settles ok:false.',
    inputSchema: {
      type: 'object',
      properties: {
        host: { type: 'string' },
        port: { type: 'integer', minimum: MIN_TCP_PORT, maximum: MAX_TCP_PORT },
        timeoutMs: { type: 'integer', minimum: 1, default: DEFAULT_PROBE_TIMEOUT_MS },
      },
      required: ['host', 'port'],
    },
    outputSchema: {
      type: 'object',
      properties: {
        ok: { type: 'boolean' },
        latencyMs: { type: 'integer', minimum: 0 },
      },
      required: ['ok'],
    },
  },
  {
    id: URL_NORMALIZE_METHOD_ID,
    name: URL_NORMALIZE_METHOD_NAME,
    description: 'Parse and normalize a URL, optionally against a base, into its parts.',
    inputSchema: {
      type: 'object',
      properties: { url: { type: 'string' }, base: { type: 'string' } },
      required: ['url'],
    },
    outputSchema: {
      type: 'object',
      properties: {
        url: { type: 'string' },
        host: { type: 'string' },
        pathname: { type: 'string' },
        searchParams: { type: 'object', additionalProperties: { type: 'string' } },
      },
      required: ['url', 'host', 'pathname', 'searchParams'],
    },
  },
];

// ---------------------------------------------------------------------------
// Injectable effect ports.
// ---------------------------------------------------------------------------

/**
 * @brief Minimal DNS lookup port backed by node:dns/promises in production.
 * @note Fake implementations return fixed string arrays or reject with an Error carrying a
 *       Node-style `code` property (ENOTFOUND, ENODATA, ...).
 */
export interface DnsResolver {
  resolve4(host: string): Promise<string[]>;
  resolve6(host: string): Promise<string[]>;
}

/**
 * @brief One in-flight TCP connection attempt; the production port wraps a net.Socket.
 * @note Callbacks fire at most once and never after destroy(); destroy() tears the
 *       connection down immediately (the probe timeout relies on this).
 */
export interface TcpConnection {
  onConnected(callback: () => void): void;
  onFailed(callback: (error: unknown) => void): void;
  destroy(): void;
}

/** @brief Factory port for one TCP connection attempt. */
export interface TcpConnector {
  connect(host: string, port: number): TcpConnection;
}

/** @brief Effect dependencies accepted by createWithDeps(); each entry is optional. */
export interface NetPluginDeps {
  readonly dns?: DnsResolver;
  readonly tcp?: TcpConnector;
}

/** @brief Real resolver over node:dns/promises; created once per module evaluation. */
const nodeDnsResolver: DnsResolver = {
  resolve4: (host: string): Promise<string[]> => resolve4(host),
  resolve6: (host: string): Promise<string[]> => resolve6(host),
};

/** @brief Real connector over node:net; created once per module evaluation. */
const nodeTcpConnector: TcpConnector = {
  connect(host: string, port: number): TcpConnection {
    const socket = createConnection({ host, port });
    const connectedCallbacks: Array<() => void> = [];
    const failedCallbacks: Array<(error: unknown) => void> = [];
    socket.once('connect', () => {
      for (const callback of [...connectedCallbacks]) callback();
    });
    socket.once('error', (error: Error) => {
      for (const callback of [...failedCallbacks]) callback(error);
    });
    return {
      onConnected(callback: () => void): void {
        connectedCallbacks.push(callback);
      },
      onFailed(callback: (error: unknown) => void): void {
        failedCallbacks.push(callback);
      },
      destroy(): void {
        // Drop every listener first, then keep one inert error sink so a post-destroy
        // socket error can never escape as an uncaught exception or reach the plugin.
        socket.removeAllListeners();
        socket.on('error', () => {});
        socket.destroy();
      },
    };
  },
};

// ---------------------------------------------------------------------------
// Argument validation helpers.
// ---------------------------------------------------------------------------

/**
 * @brief Narrow invoke args to a JSON object.
 *
 * @param value Candidate request args document.
 *
 * @param method Method name used in the error message.
 *
 * @return The same value typed as JsonObject.
 *
 * @throws RackError 'invalid_argument' when the value is not a JSON object.
 */
function expectArgsObject(value: JsonValue, method: string): JsonObject {
  // The final cast is needed because Array.isArray does not narrow away the readonly
  // JsonArray union member (readonly arrays are not assignable to any[]).
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new RackError('invalid_argument', `${method} args must be a JSON object`);
  }
  return value as JsonObject;
}

/** @brief One hostname label: alnum start/end, hyphens inside, 1-63 characters. */
const HOST_LABEL_PATTERN = /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/;

/**
 * @brief Structural hostname check (RFC 1123 labels, optional trailing FQDN dot).
 *
 * @param host Candidate host string.
 *
 * @return true for syntactically plausible hostnames such as `localhost`, `example.com.`
 *         or `api-v2.example.co.uk`.
 *
 * @note Address literals other than dotted-quad IPv4 are rejected here; resolve4/resolve6
 *       take hostnames, and IPv6 literals are accepted only by the probe path.
 */
function isValidHostname(host: string): boolean {
  const name = host.endsWith('.') ? host.slice(0, -1) : host;
  if (name.length === 0 || name.length > 253) return false;
  return name.split('.').every((label) => HOST_LABEL_PATTERN.test(label));
}

/**
 * @brief Loose IPv6 literal check for the probe path (hex digits, colons, dots, zone id).
 *
 * @param host Candidate host string.
 *
 * @return true when the value looks like an IPv6 literal such as `::1` or `fe80::1%eth0`;
 *         malformed literals simply fail to connect and settle ok:false.
 */
function isIpv6Literal(host: string): boolean {
  return host.includes(':') && /^[0-9A-Fa-f:.%]+$/.test(host);
}

/**
 * @brief Validate and return the mandatory host argument.
 *
 * @param args Request args already narrowed to a JSON object.
 *
 * @param allowIpv6 Whether IPv6 literals are acceptable (probe: yes, dns.resolve: no).
 *
 * @return The validated host string.
 *
 * @throws RackError 'invalid_argument' when args.host is missing, not a string, or not a
 *         syntactically valid host.
 */
function expectHost(args: JsonObject, allowIpv6: boolean): string {
  const host = args['host'];
  if (typeof host !== 'string') {
    throw new RackError('invalid_argument', 'args.host must be a string');
  }
  if (!isValidHostname(host) && !(allowIpv6 && isIpv6Literal(host))) {
    throw new RackError('invalid_argument', `args.host is not a valid host: ${host}`);
  }
  return host;
}

/**
 * @brief Validate and return the mandatory TCP port argument.
 *
 * @param args Request args already narrowed to a JSON object.
 *
 * @return The validated port.
 *
 * @throws RackError 'invalid_argument' when args.port is missing or not a safe integer
 *         within [1, 65535].
 */
function expectPort(args: JsonObject): number {
  const port = args['port'];
  if (
    typeof port !== 'number' ||
    !Number.isSafeInteger(port) ||
    port < MIN_TCP_PORT ||
    port > MAX_TCP_PORT
  ) {
    throw new RackError(
      'invalid_argument',
      `args.port must be an integer in [${MIN_TCP_PORT}, ${MAX_TCP_PORT}]`,
    );
  }
  return port;
}

/**
 * @brief Validate and return the optional probe timeout argument.
 *
 * @param args Request args already narrowed to a JSON object.
 *
 * @return args.timeoutMs when present, otherwise the 2000 ms default.
 *
 * @throws RackError 'invalid_argument' when a present timeoutMs is not a positive safe
 *         integer.
 */
function expectTimeoutMs(args: JsonObject): number {
  const raw = args['timeoutMs'];
  if (raw === undefined) return DEFAULT_PROBE_TIMEOUT_MS;
  if (typeof raw !== 'number' || !Number.isSafeInteger(raw) || raw <= 0) {
    throw new RackError('invalid_argument', 'args.timeoutMs must be a positive integer when present');
  }
  return raw;
}

/**
 * @brief Extract a Node-style string `code` property from an unknown rejection reason.
 *
 * @param error Rejection reason of a resolver or connection callback.
 *
 * @return The code string, or undefined when the reason carries none.
 */
function errorCodeOf(error: unknown): string | undefined {
  if (typeof error === 'object' && error !== null && 'code' in error) {
    const code = (error as { readonly code?: unknown }).code;
    if (typeof code === 'string') return code;
  }
  return undefined;
}

// ---------------------------------------------------------------------------
// Pure URL normalization.
// ---------------------------------------------------------------------------

/**
 * @brief Normalize one URL through the WHATWG URL parser.
 *
 * @param args Request args already narrowed to a JSON object ({url, base?}).
 *
 * @return {url, host, pathname, searchParams} with searchParams as a plain object;
 *         repeated query keys keep the last value.
 *
 * @throws RackError 'invalid_argument' when url/base are not usable strings or the URL
 *         parser rejects the input.
 */
function normalizeUrl(args: JsonObject): JsonObject {
  const input = args['url'];
  if (typeof input !== 'string' || input.length === 0) {
    throw new RackError('invalid_argument', 'args.url must be a non-empty string');
  }
  const base = args['base'];
  if (base !== undefined && typeof base !== 'string') {
    throw new RackError('invalid_argument', 'args.base must be a string when present');
  }
  let parsed: URL;
  try {
    parsed = base === undefined ? new URL(input) : new URL(input, base);
  } catch (error) {
    throw new RackError('invalid_argument', `args.url is not a valid URL: ${String(error)}`);
  }
  const searchParams: Record<string, string> = {};
  for (const [key, value] of parsed.searchParams) {
    searchParams[key] = value; // repeated keys: the last value wins
  }
  return {
    url: parsed.href,
    host: parsed.host,
    pathname: parsed.pathname,
    searchParams,
  };
}

// ---------------------------------------------------------------------------
// Factory.
// ---------------------------------------------------------------------------

/**
 * @brief Validate that every provided dependency port has the required functions.
 *
 * @param deps Caller-supplied dependency bundle.
 *
 * @throws RackError 'invalid_argument' when a provided port is malformed.
 */
function validateDeps(deps: NetPluginDeps): void {
  if (
    deps.dns !== undefined &&
    (typeof deps.dns.resolve4 !== 'function' || typeof deps.dns.resolve6 !== 'function')
  ) {
    throw new RackError('invalid_argument', 'deps.dns must expose resolve4/resolve6 functions');
  }
  if (deps.tcp !== undefined && typeof deps.tcp.connect !== 'function') {
    throw new RackError('invalid_argument', 'deps.tcp must expose a connect function');
  }
}

/**
 * @brief Create one std.net device instance over injectable effect ports.
 *
 * @param deps Optional DNS resolver and TCP connector; omitted entries fall back to the
 *        Node built-in implementations. Defaults to no injection at all.
 *
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 *
 * @throws RackError 'invalid_argument' when a provided dependency port is malformed.
 *
 * @note Concurrency discipline: probes are counted while in flight; stop() aborts every
 *       in-flight probe (each settles {ok: false} and its connection is destroyed
 *       immediately) and returns only after all probes settled, so no probe callback can
 *       fire after stop() completes.
 */
export function createWithDeps(deps: NetPluginDeps = {}): RackPlugin {
  validateDeps(deps);
  const dnsResolver = deps.dns ?? nodeDnsResolver;
  const tcpConnector = deps.tcp ?? nodeTcpConnector;

  // Services borrowed during init(); kept because invoke keeps serving after init returns.
  let services: PluginServices | null = null;
  // True between start() and stop(); invoke rejects with invalid_state otherwise.
  let running = false;
  // Lifetime of this instance; abort() in stop() settles every in-flight probe.
  const lifetime = new AbortController();
  // Number of probes between their start and their settlement.
  let inFlightProbes = 0;
  // Wakers parked by stop() while in-flight probes are still settling.
  const settleWaiters: Array<() => void> = [];

  /**
   * @brief Wake parked stop() calls once no in-flight probe remains.
   */
  const wakeStopWaitersWhenIdle = (): void => {
    if (!running && inFlightProbes === 0) {
      for (const wake of settleWaiters.splice(0)) wake();
    }
  };

  /**
   * @brief Merge resolve4/resolve6 results, deduplicated, v4 addresses first.
   *
   * @param host Validated hostname to resolve.
   *
   * @return {addresses} with every distinct address of the successful families.
   *
   * @throws RackError 'failed' with detail {code} when both family queries reject; the
   *         code of the first (A record) rejection is reported. A host with only one
   *         address family (for example no AAAA records) still succeeds with the addresses
   *         of the other family.
   */
  const resolveDns = async (host: string): Promise<JsonObject> => {
    const outcomes = await Promise.allSettled([
      dnsResolver.resolve4(host),
      dnsResolver.resolve6(host),
    ]);
    const addresses: string[] = [];
    const seen = new Set<string>();
    let succeededFamilies = 0;
    let firstError: unknown;
    for (const outcome of outcomes) {
      if (outcome.status === 'fulfilled') {
        succeededFamilies += 1;
        for (const address of outcome.value) {
          if (!seen.has(address)) {
            seen.add(address);
            addresses.push(address);
          }
        }
      } else if (firstError === undefined) {
        firstError = outcome.reason;
      }
    }
    if (succeededFamilies === 0) {
      const code = errorCodeOf(firstError);
      throw new RackError(
        'failed',
        `dns resolve failed for ${host}`,
        code === undefined ? undefined : { code },
      );
    }
    return { addresses };
  };

  /**
   * @brief Probe one TCP endpoint with a hard timeout.
   *
   * @param host Validated target host.
   *
   * @param port Validated target port.
   *
   * @param timeoutMs Hard deadline in milliseconds; the connection is destroyed
   *        immediately when it fires.
   *
   * @return {ok: true, latencyMs} on connect, {ok: false} on failure, timeout or abort;
   *         the promise never rejects.
   *
   * @note Every settlement path destroys the connection, clears its timer and detaches its
   *       abort listener, and late callbacks are ignored through the settled flag, so no
   *       probe callback can fire after settlement or after stop().
   */
  const probeTcp = (host: string, port: number, timeoutMs: number): Promise<JsonObject> => {
    const startedAt = Date.now();
    inFlightProbes += 1;
    return new Promise<JsonObject>((resolve) => {
      let settled = false;
      let timer: ReturnType<typeof setTimeout> | null = null;
      let onAbort: (() => void) | null = null;

      const connection = tcpConnector.connect(host, port);

      const finish = (ok: boolean): void => {
        if (settled) return;
        settled = true;
        if (timer !== null) clearTimeout(timer);
        if (onAbort !== null) lifetime.signal.removeEventListener('abort', onAbort);
        // Always tear the connection down immediately, success included.
        connection.destroy();
        inFlightProbes -= 1;
        wakeStopWaitersWhenIdle();
        resolve(
          ok
            ? { ok: true, latencyMs: Date.now() - startedAt }
            : { ok: false },
        );
      };

      connection.onConnected(() => finish(true));
      connection.onFailed(() => finish(false));

      timer = setTimeout(() => finish(false), timeoutMs);

      onAbort = () => finish(false);
      lifetime.signal.addEventListener('abort', onAbort);
      // addEventListener does not fire for an already-aborted signal; cover that race.
      if (lifetime.signal.aborted) finish(false);
    });
  };

  /**
   * @brief Dispatch one gateway request to its method handler.
   *
   * @param request Gateway request with method name/id and JSON args.
   *
   * @return The method result document.
   *
   * @throws RackError 'invalid_state' before start() or after stop(), 'invalid_argument'
   *         for malformed args, 'not_found' for unknown method names/ids.
   */
  const dispatch = async (request: InvokeRequest): Promise<JsonValue> => {
    if (!running) {
      throw new RackError('invalid_state', `std.net is not active (method ${request.methodName})`);
    }
    if (
      request.methodId === DNS_RESOLVE_METHOD_ID ||
      request.methodName === DNS_RESOLVE_METHOD_NAME
    ) {
      const args = expectArgsObject(request.args, DNS_RESOLVE_METHOD_NAME);
      return resolveDns(expectHost(args, false));
    }
    if (
      request.methodId === TCP_PROBE_METHOD_ID ||
      request.methodName === TCP_PROBE_METHOD_NAME
    ) {
      const args = expectArgsObject(request.args, TCP_PROBE_METHOD_NAME);
      const host = expectHost(args, true);
      const port = expectPort(args);
      const timeoutMs = expectTimeoutMs(args);
      return probeTcp(host, port, timeoutMs);
    }
    if (
      request.methodId === URL_NORMALIZE_METHOD_ID ||
      request.methodName === URL_NORMALIZE_METHOD_NAME
    ) {
      const args = expectArgsObject(request.args, URL_NORMALIZE_METHOD_NAME);
      return normalizeUrl(args);
    }
    throw new RackError(
      'not_found',
      `unknown net method ${request.methodName} (#${request.methodId})`,
    );
  };

  return definePlugin(
    { plugId: NET_PLUG_ID, version: NET_VERSION, init: {} },
    {
      init: (injected) => {
        services = injected;
      },
      start: () => {
        services?.caps.publish({ methods: NET_METHODS });
        running = true;
      },
      stop: async () => {
        running = false;
        // Settles every in-flight probe synchronously as {ok: false}.
        lifetime.abort();
        if (inFlightProbes > 0) {
          // Belt and braces: a settle path that is not abort-driven still parks stop()
          // until the in-flight count reaches zero.
          await new Promise<void>((wake) => {
            settleWaiters.push(wake);
          });
        }
      },
      invoke: (request) => dispatch(request),
    },
  );
}

/**
 * @brief Create one std.net device instance over the real Node built-ins.
 *
 * @return A RackPlugin using node:dns/promises, node:net and the global URL.
 *
 * @note Unit tests prefer createWithDeps() with fake ports; this factory performs real
 *       network effects.
 */
export function create(): RackPlugin {
  return createWithDeps();
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
