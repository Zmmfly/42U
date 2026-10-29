/**
 * @file Tests for the bounded queued event bus (reserved names, FIFO drain, owner revocation).
 */
import { describe, expect, it } from 'vitest';
import { createEventBus } from '../src/core/events.ts';
import type { EventSink, RackEvent } from '../src/core/types.ts';
import type { JsonValue } from '../src/core/json.ts';
import { estimateJsonBytes } from '../src/core/json.ts';
import type { RackStatus } from '../src/core/status.ts';
import { statusOf } from '../src/core/status.ts';

/** @brief Cast an arbitrary value to the payload parameter type, mirroring an untyped caller. */
const asPayload = (value: unknown): JsonValue => value as JsonValue;

/** @brief Recording sink factory capturing delivered events in order. */
function recorder(): { sink: EventSink; events: RackEvent[] } {
  const events: RackEvent[] = [];
  return { sink: { onEvent: (event) => { events.push(event); } }, events };
}

/** @brief Assert that run() throws a RackError carrying the given status. */
function expectStatus(run: () => unknown, status: RackStatus): void {
  try {
    run();
  } catch (error) {
    expect(statusOf(error)).toBe(status);
    return;
  }
  expect.unreachable();
}

describe('createEventBus', () => {
  it('rejects malformed bounds and observers', () => {
    expectStatus(() => createEventBus({ capacity: 0 }), 'invalid_argument');
    expectStatus(() => createEventBus({ capacity: 1.5 }), 'invalid_argument');
    expectStatus(() => createEventBus({ payloadLimit: -8 }), 'invalid_argument');
    expectStatus(
      () => createEventBus({ onSinkError: 3 as unknown as (name: string, error: unknown) => void }),
      'invalid_argument',
    );
  });

  it('defaults capacity to 1024 and payloadLimit to 1 MiB', async () => {
    const bus = createEventBus();
    for (let i = 0; i < 1024; i++) bus.publish('tick', i, 'publisher');
    expect(bus.pending).toBe(1024);
    expectStatus(() => bus.publish('tick', 0, 'publisher'), 'limit_exceeded');
    expect(await bus.drain(2048)).toBe(1024);
    expect(bus.pending).toBe(0);
    const oversized = 'x'.repeat(350_000);
    expect(estimateJsonBytes(oversized)).toBeGreaterThan(1024 * 1024);
    expectStatus(() => bus.publish('blob', oversized, 'publisher'), 'limit_exceeded');
    bus.publish('blob', 'small', 'publisher');
    expect(bus.pending).toBe(1);
  });
});

describe('publish validation', () => {
  it('rejects empty names', () => {
    const bus = createEventBus();
    expectStatus(() => bus.publish('', null, 'p'), 'invalid_argument');
    expectStatus(() => bus.publish(undefined as unknown as string, null, 'p'), 'invalid_argument');
  });

  it('rejects reserved u42.* names but allows lookalikes', () => {
    const bus = createEventBus();
    expectStatus(() => bus.publish('u42.capability.available', null, 'p'), 'invalid_argument');
    expectStatus(() => bus.publish('u42.', null, 'p'), 'invalid_argument');
    bus.publish('u42x.foo', null, 'p');
    bus.publish('U42.foo', null, 'p');
    bus.publish('u42', null, 'p');
    expect(bus.pending).toBe(3);
  });

  it('rejects non-JSON payloads', () => {
    const bus = createEventBus();
    const cyclic: Record<string, unknown> = {};
    cyclic.self = cyclic;
    const badValues: unknown[] = [
      Number.NaN,
      Number.POSITIVE_INFINITY,
      undefined,
      () => 1,
      10n,
      Symbol('s'),
      new Date(0),
      cyclic,
      [1, Number.NaN],
    ];
    for (const bad of badValues) {
      expectStatus(() => bus.publish('e', asPayload(bad), 'p'), 'invalid_argument');
    }
    expect(bus.pending).toBe(0);
  });

  it('rejects payloads above payloadLimit and allows the exact boundary', () => {
    const bus = createEventBus({ payloadLimit: 10 });
    expect(estimateJsonBytes('hello')).toBeGreaterThan(10);
    expect(estimateJsonBytes('a')).toBeLessThanOrEqual(10);
    expectStatus(() => bus.publish('e', 'hello', 'p'), 'limit_exceeded');
    bus.publish('e', 'a', 'p');
    expect(bus.pending).toBe(1);
    const exact = createEventBus({ payloadLimit: estimateJsonBytes('a') });
    exact.publish('e', 'a', 'p');
    expect(exact.pending).toBe(1);
  });

  it('rejects when the queue is full and recovers after drain', async () => {
    const bus = createEventBus({ capacity: 2 });
    bus.publish('e', 1, 'p');
    bus.publish('e', 2, 'p');
    expectStatus(() => bus.publish('e', 3, 'p'), 'limit_exceeded');
    expect(bus.pending).toBe(2);
    expect(await bus.drain(10)).toBe(2);
    bus.publish('e', 3, 'p');
    expect(bus.pending).toBe(1);
  });
});

describe('subscriptions', () => {
  it('validates name, sink shape and ownerId', () => {
    const bus = createEventBus();
    const sink: EventSink = { onEvent: () => {} };
    expectStatus(() => bus.subscribe('', sink, 'o'), 'invalid_argument');
    expectStatus(() => bus.subscribe('e', undefined as unknown as EventSink, 'o'), 'invalid_argument');
    expectStatus(() => bus.subscribe('e', {} as EventSink, 'o'), 'invalid_argument');
    expectStatus(() => bus.subscribe('e', sink, ''), 'invalid_argument');
  });

  it('delivers one event to every subscriber of the matching name', async () => {
    const bus = createEventBus();
    const one = recorder();
    const two = recorder();
    const other = recorder();
    bus.subscribe('ping', one.sink, 'o1');
    bus.subscribe('ping', two.sink, 'o2');
    bus.subscribe('pong', other.sink, 'o3');
    bus.publish('ping', 7, 'p');
    bus.publish('pong', 8, 'p');
    expect(await bus.drain(10)).toBe(2);
    expect(one.events.map((e) => e.payload)).toEqual([7]);
    expect(two.events.map((e) => e.payload)).toEqual([7]);
    expect(other.events.map((e) => e.name)).toEqual(['pong']);
  });

  it('stops delivery after unsubscribe and stays idempotent', async () => {
    const bus = createEventBus();
    const gone = recorder();
    const kept = recorder();
    const sub = bus.subscribe('e', gone.sink, 'o1');
    bus.subscribe('e', kept.sink, 'o2');
    sub.unsubscribe();
    sub.unsubscribe();
    bus.publish('e', 1, 'p');
    expect(await bus.drain(1)).toBe(1);
    expect(gone.events).toHaveLength(0);
    expect(kept.events.map((e) => e.payload)).toEqual([1]);
  });
});

describe('unsubscribeAll', () => {
  it('revokes every subscription of one owner and leaves other owners intact', async () => {
    const bus = createEventBus();
    const a1 = recorder();
    const a2 = recorder();
    const b = recorder();
    bus.subscribe('x', a1.sink, 'A');
    bus.subscribe('y', a2.sink, 'A');
    bus.subscribe('x', b.sink, 'B');
    bus.unsubscribeAll('A');
    bus.unsubscribeAll('A');
    bus.unsubscribeAll('nobody');
    bus.publish('x', 1, 'p');
    bus.publish('y', 2, 'p');
    expect(await bus.drain(10)).toBe(2);
    expect(a1.events).toHaveLength(0);
    expect(a2.events).toHaveLength(0);
    expect(b.events.map((e) => e.payload)).toEqual([1]);
  });
});

describe('drain ordering and budget', () => {
  it('delivers in global FIFO order across event names', async () => {
    const log: string[] = [];
    const bus = createEventBus();
    bus.subscribe('left', { onEvent: (e) => { log.push(`left:${e.payload}`); } }, 'o1');
    bus.subscribe('right', { onEvent: (e) => { log.push(`right:${e.payload}`); } }, 'o2');
    bus.publish('left', 1, 'p');
    bus.publish('right', 2, 'p');
    bus.publish('left', 3, 'p');
    expect(await bus.drain(10)).toBe(3);
    expect(log).toEqual(['left:1', 'right:2', 'left:3']);
  });

  it('delivers only part of the queue when the budget runs out', async () => {
    const bus = createEventBus();
    const first = recorder();
    bus.subscribe('e', first.sink, 'o');
    for (let i = 0; i < 5; i++) bus.publish('e', i, 'p');
    expect(await bus.drain(0)).toBe(0);
    expect(await bus.drain(-5)).toBe(0);
    expect(await bus.drain(Number.NaN)).toBe(0);
    expect(bus.pending).toBe(5);
    expect(await bus.drain(2)).toBe(2);
    expect(bus.pending).toBe(3);
    expect(first.events.map((e) => e.payload)).toEqual([0, 1]);
    expect(await bus.drain(99)).toBe(3);
    expect(bus.pending).toBe(0);
    expect(first.events.map((e) => e.payload)).toEqual([0, 1, 2, 3, 4]);
  });
});

describe('drain with async sinks', () => {
  it('awaits async sinks serially before the next event', async () => {
    const log: string[] = [];
    const bus = createEventBus();
    bus.subscribe(
      'job',
      {
        onEvent: async (e) => {
          log.push(`start:${e.payload}`);
          await new Promise((resolve) => setTimeout(resolve, 1));
          log.push(`end:${e.payload}`);
        },
      },
      'worker',
    );
    bus.publish('job', 1, 'p');
    bus.publish('job', 2, 'p');
    expect(await bus.drain(10)).toBe(2);
    expect(log).toEqual(['start:1', 'end:1', 'start:2', 'end:2']);
  });

  it('resolves re-entrant drain from inside a sink to 0', async () => {
    const inner: number[] = [];
    const log: string[] = [];
    const bus = createEventBus();
    bus.subscribe(
      'e',
      {
        onEvent: async (e) => {
          log.push(`s:${e.payload}`);
          inner.push(await bus.drain(5));
          log.push(`e:${e.payload}`);
        },
      },
      'o',
    );
    bus.publish('e', 1, 'p');
    bus.publish('e', 2, 'p');
    expect(await bus.drain(10)).toBe(2);
    expect(inner).toEqual([0, 0]);
    expect(log).toEqual(['s:1', 'e:1', 's:2', 'e:2']);
  });

  it('picks up events published inside a sink callback in the same serial pass', async () => {
    const order: string[] = [];
    const bus = createEventBus();
    bus.subscribe('a', { onEvent: () => { order.push('a'); bus.publish('b', null, 'p'); } }, 'o');
    bus.subscribe('b', { onEvent: () => { order.push('b'); } }, 'o');
    bus.publish('a', null, 'p');
    expect(await bus.drain(10)).toBe(2);
    expect(order).toEqual(['a', 'b']);
  });
});

describe('sink failures', () => {
  it('routes sink throws and rejections to onSinkError and keeps dispatching', async () => {
    const failures: Array<{ name: string; error: unknown }> = [];
    const boom = new Error('boom');
    const rejection = new Error('async boom');
    const bus = createEventBus({
      onSinkError: (name, error) => { failures.push({ name, error }); },
    });
    const survivor = recorder();
    bus.subscribe('x', { onEvent: () => { throw boom; } }, 'thrower');
    bus.subscribe('x', { onEvent: async () => { throw rejection; } }, 'rejecter');
    bus.subscribe('x', survivor.sink, 'survivor');
    bus.publish('x', 1, 'p');
    bus.publish('x', 2, 'p');
    expect(await bus.drain(10)).toBe(2);
    expect(survivor.events.map((e) => e.payload)).toEqual([1, 2]);
    expect(failures.map((f) => [f.name, f.error])).toEqual([
      ['x', boom],
      ['x', rejection],
      ['x', boom],
      ['x', rejection],
    ]);
  });

  it('swallows sink failures when no onSinkError is configured', async () => {
    const bus = createEventBus();
    const survivor = recorder();
    bus.subscribe('x', { onEvent: () => { throw new Error('boom'); } }, 'thrower');
    bus.subscribe('x', { onEvent: async () => { throw new Error('async boom'); } }, 'rejecter');
    bus.subscribe('x', survivor.sink, 'survivor');
    bus.publish('x', 1, 'p');
    await expect(bus.drain(10)).resolves.toBe(1);
    expect(survivor.events.map((e) => e.payload)).toEqual([1]);
  });
});

describe('payload references', () => {
  it('passes the published payload reference through without copying', async () => {
    const bus = createEventBus();
    const seen: JsonValue[] = [];
    bus.subscribe('doc', { onEvent: (e) => { seen.push(e.payload); } }, 'o');
    const payload: JsonValue = { items: [1, 2] };
    bus.publish('doc', payload, 'p');
    expect(await bus.drain(1)).toBe(1);
    expect(seen[0]).toBe(payload);
  });
});
