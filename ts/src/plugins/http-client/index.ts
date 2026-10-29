/**
 * @file index.ts
 * @brief Standard HTTP client device (std.http-client): a production-grade wrapper over the
 *        global fetch with per-attempt timeouts, bounded retries with linear backoff and a
 *        streaming response-body size guard.
 *
 * @details Everything on the rack is a plugin, so this standard device is consumed like any
 *          other provider: lease it, then call request/get/post/put/del through the host
 *          invoke gateway. Zero npm dependencies: only the global fetch, the global
 *          AbortController and WHATWG globals (URL, TextDecoder, TextEncoder) are used. All
 *          external effects are injectable through createWithDeps({ fetchImpl, maxBytes }) so
 *          tests stay hermetic; create() binds globalThis.fetch.
 *
 *          Retry policy: only network-layer failures (fetch rejections, including timeouts)
 *          and 5xx responses consume the retry budget; 4xx responses are returned as-is. The
 *          timeout covers headers plus the full body read of one attempt, and stop() aborts
 *          every in-flight attempt and cancels pending backoff waits.
 */

import { definePlugin, isRackError, RackError } from '../../sdk/index.ts';
import type {
  InvokeRequest,
  JsonObject,
  JsonValue,
  MethodDesc,
  PluginDescriptor,
  PluginServices,
  PluginVersion,
  RackPlugin,
} from '../../sdk/index.ts';

// ---------------------------------------------------------------------------
// Published contract.
// ---------------------------------------------------------------------------

/** @brief Canonical plugin identity announced to the host. */
export const HTTP_CLIENT_PLUG_ID = 'std.http-client';

/** @brief Business version published by this device. */
export const HTTP_CLIENT_VERSION: PluginVersion = { major: 1, minor: 0, patch: 0 };

/** @brief Per-attempt timeout default in milliseconds. */
const DEFAULT_TIMEOUT_MS = 10_000;

/** @brief Default number of additional attempts after the first one. */
const DEFAULT_RETRIES = 0;

/** @brief Default linear backoff base in milliseconds. */
const DEFAULT_RETRY_BACKOFF_MS = 100;

/** @brief Default cumulative response-body limit: 4 MiB. */
const DEFAULT_MAX_BYTES = 4 * 1024 * 1024;

/** @brief Environment variable overriding the default response-body limit. */
const MAX_BYTES_ENV = 'U42_HTTP_MAX_BYTES';

/** @brief Maximum length of the body snippet attached to expectOk failures. */
const EXPECT_OK_SNIPPET_CHARS = 200;

/** @brief HTTP verbs accepted for args.method (compared case-insensitively). */
const HTTP_VERBS: ReadonlySet<string> = new Set([
  'GET',
  'HEAD',
  'POST',
  'PUT',
  'DELETE',
  'CONNECT',
  'OPTIONS',
  'TRACE',
  'PATCH',
]);

/** @brief JSON-schema-shaped description of the full request args document. */
const REQUEST_INPUT_SCHEMA: JsonValue = {
  type: 'object',
  properties: {
    url: { type: 'string', description: 'Absolute http(s) URL.' },
    method: { type: 'string', description: 'HTTP verb, case-insensitive; defaults to GET.' },
    headers: { type: 'object', additionalProperties: { type: 'string' } },
    json: { description: 'JSON request body; automatically sets content-type application/json.' },
    text: { type: 'string', description: 'Plain string request body.' },
    timeoutMs: { type: 'number', description: 'Per-attempt timeout; default 10000.' },
    retries: { type: 'number', description: 'Additional attempts after the first; default 0.' },
    retryBackoffMs: {
      type: 'number',
      description: 'Linear backoff base; the n-th failure waits n times this value; default 100.',
    },
    expectOk: {
      type: 'boolean',
      description: 'Fail with RackError failed when the final status is not 2xx; default false.',
    },
  },
  required: ['url'],
};

/** @brief JSON-schema-shaped description of the shortcut args documents (no method field). */
const SHORTCUT_INPUT_SCHEMA: JsonValue = {
  type: 'object',
  properties: {
    url: { type: 'string', description: 'Absolute http(s) URL.' },
    headers: { type: 'object', additionalProperties: { type: 'string' } },
    json: { description: 'JSON request body; automatically sets content-type application/json.' },
    text: { type: 'string', description: 'Plain string request body.' },
    timeoutMs: { type: 'number', description: 'Per-attempt timeout; default 10000.' },
    retries: { type: 'number', description: 'Additional attempts after the first; default 0.' },
    retryBackoffMs: {
      type: 'number',
      description: 'Linear backoff base; the n-th failure waits n times this value; default 100.',
    },
    expectOk: {
      type: 'boolean',
      description: 'Fail with RackError failed when the final status is not 2xx; default false.',
    },
  },
  required: ['url'],
};

/** @brief JSON-schema-shaped description of every method result document. */
const RESULT_OUTPUT_SCHEMA: JsonValue = {
  type: 'object',
  description: 'status, ok, headers plus either json (JSON bodies) or text (everything else).',
};

/** @brief One published method plus the fixed HTTP verb of the shortcut methods. */
interface MethodSpec {
  readonly id: number;
  readonly name: string;
  readonly description: string;
  readonly inputSchema: JsonValue;
  /** @brief HTTP verb forced by shortcut methods; absent for the generic request method. */
  readonly fixedMethod?: string;
}

/** @brief Published method table; numeric ids are assigned contiguously 1..5. */
const METHOD_SPECS: readonly MethodSpec[] = [
  {
    id: 1,
    name: 'request',
    description:
      'Perform one HTTP request with per-attempt timeout, bounded retries and a response size guard.',
    inputSchema: REQUEST_INPUT_SCHEMA,
  },
  {
    id: 2,
    name: 'get',
    description: 'Shortcut for request with the HTTP method fixed to GET.',
    inputSchema: SHORTCUT_INPUT_SCHEMA,
    fixedMethod: 'GET',
  },
  {
    id: 3,
    name: 'post',
    description: 'Shortcut for request with the HTTP method fixed to POST.',
    inputSchema: SHORTCUT_INPUT_SCHEMA,
    fixedMethod: 'POST',
  },
  {
    id: 4,
    name: 'put',
    description: 'Shortcut for request with the HTTP method fixed to PUT.',
    inputSchema: SHORTCUT_INPUT_SCHEMA,
    fixedMethod: 'PUT',
  },
  {
    id: 5,
    name: 'del',
    description: 'Shortcut for request with the HTTP method fixed to DELETE.',
    inputSchema: SHORTCUT_INPUT_SCHEMA,
    fixedMethod: 'DELETE',
  },
];

/** @brief Methods announced through the capability service during start(). */
export const HTTP_CLIENT_METHODS: readonly MethodDesc[] = METHOD_SPECS.map((spec) => ({
  id: spec.id,
  name: spec.name,
  description: spec.description,
  inputSchema: spec.inputSchema,
  outputSchema: RESULT_OUTPUT_SCHEMA,
}));

/** @brief Immutable factory metadata. */
const HTTP_CLIENT_DESC: PluginDescriptor = {
  plugId: HTTP_CLIENT_PLUG_ID,
  version: HTTP_CLIENT_VERSION,
  init: {},
};

// ---------------------------------------------------------------------------
// Injected dependencies.
// ---------------------------------------------------------------------------

/** @brief The subset of the fetch init document this device needs. */
export interface FetchInitLike {
  readonly method?: string;
  readonly headers?: Record<string, string>;
  readonly body?: string;
  readonly signal?: AbortSignal;
}

/**
 * @brief Injectable fetch implementation used for every attempt.
 *
 * @note Fakes record calls and return programmable or hanging responses; the default
 *       implementation forwards to globalThis.fetch.
 */
export type FetchLike = (input: string, init?: FetchInitLike) => Promise<Response>;

/** @brief Externally injectable dependencies of one std.http-client instance. */
export interface HttpClientDeps {
  /** @brief Fetch implementation; defaults to globalThis.fetch. */
  readonly fetchImpl?: FetchLike;
  /** @brief Cumulative response-body byte limit; overrides U42_HTTP_MAX_BYTES. */
  readonly maxBytes?: number;
}

/**
 * @brief Bind the default fetch implementation without capturing it at module load time.
 *
 * @param input Request URL.
 * @param init Request init document.
 * @return The global fetch promise.
 */
const defaultFetch: FetchLike = (input, init) => globalThis.fetch(input, init);

// ---------------------------------------------------------------------------
// Parsed request document and module-scope helpers.
// ---------------------------------------------------------------------------

/** @brief One fully validated request; every field carries its resolved default. */
interface ParsedRequest {
  readonly url: string;
  readonly method: string;
  readonly headers: Record<string, string>;
  readonly body: string | undefined;
  readonly timeoutMs: number;
  readonly retries: number;
  readonly retryBackoffMs: number;
  readonly expectOk: boolean;
}

/** @brief Shared UTF-8 encoder for the rare null-stream body measurement. */
const UTF8_ENCODER = new TextEncoder();

/**
 * @brief Narrow a JSON value to a JSON object.
 *
 * @param value Candidate request args document.
 * @param what Name of the checked document for the error message.
 * @return The same value typed as JsonObject.
 * @throws RackError 'invalid_argument' when the value is not a JSON object.
 */
function expectArgsObject(value: JsonValue, what: string): JsonObject {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new RackError('invalid_argument', `${what} must be a JSON object`);
  }
  return value as JsonObject;
}

/**
 * @brief Parse and validate the request URL.
 *
 * @param raw Candidate URL string.
 * @return The parsed URL; its href is used as the fetch input.
 * @throws RackError 'invalid_argument' when the string is not a URL or not http(s).
 */
function parseHttpUrl(raw: string): URL {
  let parsed: URL;
  try {
    parsed = new URL(raw);
  } catch {
    throw new RackError('invalid_argument', `args.url is not a valid URL: ${raw}`);
  }
  if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
    throw new RackError('invalid_argument', `args.url must use http or https: ${raw}`);
  }
  return parsed;
}

/**
 * @brief Check whether a header map already defines content-type (any case).
 *
 * @param headers Header map to inspect.
 * @return true when a content-type entry exists.
 */
function hasContentType(headers: Record<string, string>): boolean {
  for (const key of Object.keys(headers)) {
    if (key.toLowerCase() === 'content-type') return true;
  }
  return false;
}

/**
 * @brief Extract a positive integer option.
 *
 * @param value Raw args field value; undefined means "use the default".
 * @param field Field name for the error message.
 * @return The validated integer, or undefined when absent.
 * @throws RackError 'invalid_argument' when present but not a positive safe integer.
 */
function parsePositiveInt(value: JsonValue | undefined, field: string): number | undefined {
  if (value === undefined) return undefined;
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value <= 0) {
    throw new RackError('invalid_argument', `${field} must be a positive integer`);
  }
  return value;
}

/**
 * @brief Extract a non-negative integer option.
 *
 * @param value Raw args field value; undefined means "use the default".
 * @param field Field name for the error message.
 * @return The validated integer, or undefined when absent.
 * @throws RackError 'invalid_argument' when present but not a non-negative safe integer.
 */
function parseNonNegativeInt(value: JsonValue | undefined, field: string): number | undefined {
  if (value === undefined) return undefined;
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < 0) {
    throw new RackError('invalid_argument', `${field} must be a non-negative integer`);
  }
  return value;
}

/**
 * @brief Validate one complete args document and apply all defaults.
 *
 * @param args Raw request args document.
 * @param methodName Invoked method name, used in error messages.
 * @param fixedMethod HTTP verb forced by shortcut methods; undefined for request.
 * @return The fully resolved request description.
 * @throws RackError 'invalid_argument' for every shape violation documented in the README.
 */
function parseRequestArgs(
  args: JsonValue,
  methodName: string,
  fixedMethod?: string,
): ParsedRequest {
  const obj = expectArgsObject(args, `${methodName} args`);
  if (fixedMethod !== undefined && Object.hasOwn(obj, 'method')) {
    throw new RackError(
      'invalid_argument',
      `${methodName} fixes the HTTP method; use request for a custom method`,
    );
  }
  const rawMethod = fixedMethod ?? (obj['method'] === undefined ? 'GET' : obj['method']);
  if (typeof rawMethod !== 'string') {
    throw new RackError('invalid_argument', 'args.method must be a string');
  }
  const method = rawMethod.toUpperCase();
  if (!HTTP_VERBS.has(method)) {
    throw new RackError('invalid_argument', `args.method is not an HTTP verb: ${rawMethod}`);
  }

  const rawUrl = obj['url'];
  if (typeof rawUrl !== 'string' || rawUrl.length === 0) {
    throw new RackError('invalid_argument', 'args.url must be a non-empty string');
  }
  const url = parseHttpUrl(rawUrl).href;

  const headers: Record<string, string> = {};
  const rawHeaders = obj['headers'];
  if (rawHeaders !== undefined) {
    if (typeof rawHeaders !== 'object' || rawHeaders === null || Array.isArray(rawHeaders)) {
      throw new RackError('invalid_argument', 'args.headers must be a JSON object');
    }
    for (const [key, value] of Object.entries(rawHeaders)) {
      if (key.length === 0) {
        throw new RackError('invalid_argument', 'args.header names must be non-empty strings');
      }
      if (typeof value !== 'string') {
        throw new RackError('invalid_argument', `args.headers["${key}"] must be a string`);
      }
      headers[key] = value;
    }
  }

  const hasJson = Object.hasOwn(obj, 'json');
  const hasText = Object.hasOwn(obj, 'text');
  if (hasJson && hasText) {
    throw new RackError('invalid_argument', 'args.json and args.text are mutually exclusive');
  }
  let body: string | undefined;
  if (hasJson) {
    body = JSON.stringify(obj['json']);
    if (!hasContentType(headers)) headers['content-type'] = 'application/json';
  } else if (hasText) {
    const rawText = obj['text'];
    if (typeof rawText !== 'string') {
      throw new RackError('invalid_argument', 'args.text must be a string');
    }
    body = rawText;
  }
  if (body !== undefined && (method === 'GET' || method === 'HEAD')) {
    throw new RackError('invalid_argument', `${method} requests cannot carry a body`);
  }

  const rawExpectOk = obj['expectOk'];
  if (rawExpectOk !== undefined && typeof rawExpectOk !== 'boolean') {
    throw new RackError('invalid_argument', 'args.expectOk must be a boolean');
  }

  return {
    url,
    method,
    headers,
    body,
    timeoutMs: parsePositiveInt(obj['timeoutMs'], 'args.timeoutMs') ?? DEFAULT_TIMEOUT_MS,
    retries: parseNonNegativeInt(obj['retries'], 'args.retries') ?? DEFAULT_RETRIES,
    retryBackoffMs:
      parseNonNegativeInt(obj['retryBackoffMs'], 'args.retryBackoffMs') ??
      DEFAULT_RETRY_BACKOFF_MS,
    expectOk: rawExpectOk === true,
  };
}

/**
 * @brief Try to parse a response body as JSON according to the content type.
 *
 * @param contentType Response content-type header value, or null when absent.
 * @param text Fully read response body.
 * @return The parsed value, or null when the type is not JSON, the body is empty or parsing
 *         fails (lenient fallback to the raw text).
 */
function tryParseJsonBody(contentType: string | null, text: string): { value: JsonValue } | null {
  if (contentType === null || !contentType.toLowerCase().includes('json') || text.length === 0) {
    return null;
  }
  try {
    return { value: JSON.parse(text) as JsonValue };
  } catch {
    return null;
  }
}

/**
 * @brief Convert response headers into the result JSON object.
 *
 * @param headers Response headers.
 * @return Plain object with lowercased header names; multiple set-cookie lines are joined
 *         with ", " when the runtime exposes them separately.
 */
function responseHeadersToObject(headers: Headers): JsonObject {
  const out: Record<string, string> = {};
  headers.forEach((value: string, key: string) => {
    out[key] = value;
  });
  const cookies = headers.getSetCookie();
  if (cookies.length > 1) out['set-cookie'] = cookies.join(', ');
  return out;
}

/**
 * @brief Render an unknown thrown value as a short diagnostic string.
 *
 * @param error Value caught around one fetch attempt.
 * @return Error message or stringified value.
 */
function describeError(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

// ---------------------------------------------------------------------------
// Instance factory.
// ---------------------------------------------------------------------------

/** @brief A cancellable backoff wait registered for stop(). */
interface PendingBackoff {
  cancel(): void;
}

/** @brief Outcome of one fetch attempt: a final response, or a retryable 5xx. */
type AttemptOutcome =
  | { readonly kind: 'final'; readonly response: Response; readonly text: string }
  | { readonly kind: 'retryable'; readonly response: Response };

/**
 * @brief Resolve the response-body byte limit.
 *
 * @param explicit Explicit override from createWithDeps; when present it must be a positive
 *        safe integer.
 * @return The effective cumulative body limit in bytes.
 * @throws RackError 'invalid_argument' when an explicit override is not a positive integer.
 * @note Without an explicit override U42_HTTP_MAX_BYTES is honoured when it holds a positive
 *       integer; anything else (including unset) falls back to 4 MiB.
 */
function resolveMaxBytes(explicit: number | undefined): number {
  if (explicit !== undefined) {
    if (!Number.isSafeInteger(explicit) || explicit <= 0) {
      throw new RackError('invalid_argument', 'deps.maxBytes must be a positive integer');
    }
    return explicit;
  }
  const raw = process.env[MAX_BYTES_ENV];
  if (raw !== undefined) {
    const parsed = Number(raw);
    if (Number.isSafeInteger(parsed) && parsed > 0) return parsed;
  }
  return DEFAULT_MAX_BYTES;
}

/**
 * @brief Create one std.http-client instance with injectable dependencies.
 *
 * @param deps Optional fetch implementation and body limit override.
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 * @throws RackError 'invalid_argument' when deps are malformed.
 * @note stop() aborts every in-flight attempt, cancels pending backoff waits and resolves
 *       only after all tracked invokes have settled; afterwards invoke rejects defensively
 *       with 'invalid_state' (the host state gate is the primary barrier).
 */
export function createWithDeps(deps: HttpClientDeps = {}): RackPlugin {
  if (deps.fetchImpl !== undefined && typeof deps.fetchImpl !== 'function') {
    throw new RackError('invalid_argument', 'deps.fetchImpl must be a function');
  }
  const fetchImpl: FetchLike = deps.fetchImpl ?? defaultFetch;
  const maxBytes = resolveMaxBytes(deps.maxBytes);

  let services: PluginServices | null = null;
  // Set by stop() and never reset: instances never restart per the rack contract.
  let closed = false;
  const activeControllers = new Set<AbortController>();
  const pendingBackoffs = new Set<PendingBackoff>();
  const inflightRequests = new Set<Promise<JsonValue>>();

  /**
   * @brief Build the abort reason used when stop() tears down in-flight attempts.
   *
   * @return Fresh RackError consumed by the aborted fetch as its signal reason.
   */
  const stopAbortError = (): RackError =>
    new RackError('failed', 'request aborted: std.http-client is stopping', { reason: 'stop' });

  /**
   * @brief Wait one linear backoff delay, cancellable by stop().
   *
   * @param ms Delay in milliseconds (attempt number times retryBackoffMs).
   * @return Promise resolving after the delay.
   * @throws RackError 'failed' (reason 'stop') when stop() cancels the wait.
   */
  function scheduleBackoff(ms: number): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      let timer: ReturnType<typeof globalThis.setTimeout> | undefined;
      const entry: PendingBackoff = {
        cancel(): void {
          if (timer === undefined) return;
          globalThis.clearTimeout(timer);
          timer = undefined;
          pendingBackoffs.delete(entry);
          reject(stopAbortError());
        },
      };
      timer = globalThis.setTimeout(() => {
        pendingBackoffs.delete(entry);
        resolve();
      }, ms);
      pendingBackoffs.add(entry);
    });
  }

  /**
   * @brief Read one response body while enforcing the cumulative byte limit.
   *
   * @param response Response whose body is read.
   * @param limitBytes Cumulative byte limit.
   * @param parsed Request description used for the error detail.
   * @return The decoded UTF-8 body text.
   * @throws RackError 'limit_exceeded' when the accumulated chunks pass the limit; the reader
   *         is cancelled first so the underlying stream is interrupted.
   */
  async function readBodyWithLimit(
    response: Response,
    limitBytes: number,
    parsed: ParsedRequest,
  ): Promise<string> {
    const overLimitError = (): RackError =>
      new RackError(
        'limit_exceeded',
        `${parsed.method} ${parsed.url} response body passed ${limitBytes} bytes`,
        { limitBytes, method: parsed.method, url: parsed.url, status: response.status },
      );
    const stream = response.body;
    if (stream === null) {
      // No stream to guard incrementally (e.g. 204); the buffered text is measured once.
      const buffered = await response.text();
      if (UTF8_ENCODER.encode(buffered).length > limitBytes) throw overLimitError();
      return buffered;
    }
    const reader = stream.getReader();
    const decoder = new TextDecoder('utf-8');
    let received = 0;
    let text = '';
    let interrupted = false;
    try {
      for (;;) {
        const { done, value } = await reader.read();
        if (done === true) break;
        if (value === undefined) continue;
        received += value.byteLength;
        if (received > limitBytes) {
          interrupted = true;
          try {
            await reader.cancel();
          } catch {
            // Best effort interruption; the limit error is the meaningful outcome.
          }
          throw overLimitError();
        }
        text += decoder.decode(value, { stream: true });
      }
      return text + decoder.decode();
    } catch (error) {
      if (!interrupted) {
        try {
          await reader.cancel();
        } catch {
          // Stream already closed or released.
        }
      }
      throw error;
    }
  }

  /**
   * @brief Run exactly one fetch attempt under its own timeout.
   *
   * @param parsed Fully resolved request description.
   * @param attempt Current attempt number, starting at 1.
   * @param maxAttempts Total attempt budget (1 + retries).
   * @return A final response with its guarded body text, or a retryable 5xx whose body was
   *         not read.
   * @throws Whatever fetch or the body read rejected with; the caller classifies it.
   * @note The timeout timer stays armed from before fetch until the body is fully read, so a
   *       slow body aborts the stream and fails the attempt like any network error.
   */
  async function runAttempt(
    parsed: ParsedRequest,
    attempt: number,
    maxAttempts: number,
  ): Promise<AttemptOutcome> {
    const controller = new AbortController();
    activeControllers.add(controller);
    const timer = globalThis.setTimeout(() => {
      controller.abort(
        new RackError('failed', `request timed out after ${parsed.timeoutMs}ms`, {
          reason: 'timeout',
        }),
      );
    }, parsed.timeoutMs);
    try {
      const response = await fetchImpl(parsed.url, {
        method: parsed.method,
        headers: parsed.headers,
        body: parsed.body,
        signal: controller.signal,
      });
      const retryable = !closed && attempt < maxAttempts && response.status >= 500;
      if (retryable) return { kind: 'retryable', response };
      const text = await readBodyWithLimit(response, maxBytes, parsed);
      return { kind: 'final', response, text };
    } finally {
      globalThis.clearTimeout(timer);
      activeControllers.delete(controller);
    }
  }

  /**
   * @brief Build the result document or the expectOk failure for a final response.
   *
   * @param parsed Fully resolved request description.
   * @param response Final response (retry budget spent or not retryable).
   * @param text Guarded body text.
   * @return Result JSON object with status, ok, headers and json or text.
   * @throws RackError 'failed' when expectOk is set and the status is not 2xx; the detail
   *         carries the status plus a body snippet.
   */
  function buildFinalResult(parsed: ParsedRequest, response: Response, text: string): JsonObject {
    const parsedJson = tryParseJsonBody(response.headers.get('content-type'), text);
    if (parsed.expectOk && !response.ok) {
      const summarySource = parsedJson !== null ? JSON.stringify(parsedJson.value) : text;
      throw new RackError('failed', `${parsed.method} ${parsed.url} returned HTTP ${response.status}`, {
        status: response.status,
        body: summarySource.slice(0, EXPECT_OK_SNIPPET_CHARS),
      });
    }
    const result: Record<string, JsonValue> = {
      status: response.status,
      ok: response.ok,
      headers: responseHeadersToObject(response.headers),
    };
    if (parsedJson !== null) result['json'] = parsedJson.value;
    else result['text'] = text;
    return result;
  }

  /**
   * @brief Execute one request across the whole retry budget.
   *
   * @param parsed Fully resolved request description.
   * @return The final result document.
   * @throws RackError 'limit_exceeded' from the body guard (never retried), RackError
   *         'failed' after the network-layer retry budget is exhausted (detail carries
   *         attempts) and whatever stop() raises while tearing down.
   */
  async function performRequest(parsed: ParsedRequest): Promise<JsonObject> {
    const maxAttempts = 1 + parsed.retries;
    for (let attempt = 1; ; attempt += 1) {
      let outcome: AttemptOutcome;
      try {
        outcome = await runAttempt(parsed, attempt, maxAttempts);
      } catch (error) {
        if (isRackError(error) && error.status === 'limit_exceeded') throw error;
        if (closed) throw stopAbortError();
        if (attempt < maxAttempts) {
          await scheduleBackoff(attempt * parsed.retryBackoffMs);
          continue;
        }
        throw new RackError('failed', `${parsed.method} ${parsed.url} failed after ${attempt} attempts`, {
          attempts: attempt,
          method: parsed.method,
          url: parsed.url,
          lastError: describeError(error),
        });
      }
      if (outcome.kind === 'retryable') {
        // Free the connection without reading the discarded 5xx body.
        void outcome.response.body?.cancel().catch(() => {});
        await scheduleBackoff(attempt * parsed.retryBackoffMs);
        continue;
      }
      return buildFinalResult(parsed, outcome.response, outcome.text);
    }
  }

  /**
   * @brief Route one invoke to its method and run it.
   *
   * @param request Gateway-delivered invoke request.
   * @return The method result document.
   * @throws RackError 'not_found' for unknown method names or ids.
   */
  async function dispatch(request: InvokeRequest): Promise<JsonValue> {
    const spec = METHOD_SPECS.find(
      (entry) => entry.id === request.methodId || entry.name === request.methodName,
    );
    if (spec === undefined) {
      throw new RackError(
        'not_found',
        `unknown http-client method ${request.methodName} (#${request.methodId})`,
      );
    }
    return performRequest(parseRequestArgs(request.args, spec.name, spec.fixedMethod));
  }

  return definePlugin(HTTP_CLIENT_DESC, {
    init: (injected) => {
      services = injected;
    },
    start: () => {
      services?.caps.publish({ methods: HTTP_CLIENT_METHODS });
    },
    stop: async () => {
      closed = true;
      for (const controller of [...activeControllers]) controller.abort(stopAbortError());
      for (const pending of [...pendingBackoffs]) pending.cancel();
      const outstanding = [...inflightRequests];
      if (outstanding.length > 0) await Promise.allSettled(outstanding);
    },
    destroy: () => {
      activeControllers.clear();
      pendingBackoffs.clear();
      inflightRequests.clear();
    },
    invoke: (request) => {
      if (closed) {
        throw new RackError(
          'invalid_state',
          'std.http-client is stopped and no longer accepting calls',
        );
      }
      const promise = dispatch(request);
      const tracked = promise.finally(() => {
        inflightRequests.delete(tracked);
      });
      inflightRequests.add(tracked);
      return tracked;
    },
  });
}

/**
 * @brief Create one std.http-client instance bound to globalThis.fetch.
 *
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 * @note The response-body limit comes from U42_HTTP_MAX_BYTES or the 4 MiB default.
 */
export function create(): RackPlugin {
  return createWithDeps();
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
