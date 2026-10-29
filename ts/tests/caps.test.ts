/**
 * @file Tests for the capability registry (publication, withdrawal, snapshot+watch).
 */
import { describe, expect, it } from 'vitest';
import { createCapabilityRegistry } from '../src/core/caps.ts';
import { statusOf } from '../src/core/status.ts';
import type { CapEntry, CapEvent, CapSink, CapsDesc, MethodDesc, PluginVersion } from '../src/core/types.ts';

const v = (major = 1, minor = 0, patch = 0): PluginVersion => ({ major, minor, patch });
const m = (id: number, name: string): MethodDesc => ({ id, name });
const e = (
  plugId: string,
  generation = 0,
  methods: readonly MethodDesc[] = [m(1, 'run')],
  version: PluginVersion = v(),
): CapEntry => ({ plugId, generation, version, caps: { methods } });

/** @brief Recording capability sink with injectable failure behaviour. */
function recorder(id: string, fail?: 'throw' | 'reject') {
  const events: CapEvent[] = [];
  const sink: CapSink = {
    onCapability(event: CapEvent): void | Promise<void> {
      events.push(event);
      if (fail === 'throw') throw new Error(`boom from ${id}`);
      if (fail === 'reject') return Promise.reject(new Error(`async boom from ${id}`));
    },
  };
  return { sink, events };
}

/** @brief Order-sensitive projection of received facts. */
const facts = (events: readonly CapEvent[]): { plugId: string; available: boolean }[] =>
  events.map((event) => ({ plugId: event.plugId, available: event.available }));

/** @brief Flush all pending microtasks (rejected-promise reporting chains). */
const flushMicrotasks = (): Promise<void> => new Promise((resolve) => setTimeout(resolve, 0));

describe('register + notification', () => {
  it('registers and synchronously notifies active watchers', () => {
    const registry = createCapabilityRegistry();
    const w = recorder('w');
    registry.watch(w.sink, 'w');
    registry.register(e('storage', 4, [m(1, 'get'), m(2, 'put')], v(2, 1, 0)));
    expect(w.events.length).toBe(1);
    const [event] = w.events;
    expect(event?.plugId).toBe('storage');
    expect(event?.available).toBe(true);
    expect(event?.version).toEqual({ major: 2, minor: 1, patch: 0 });
    expect(event?.capabilities.methods).toEqual([{ id: 1, name: 'get' }, { id: 2, name: 'put' }]);
  });

  it('freezes a private copy of the announcement', () => {
    const registry = createCapabilityRegistry();
    const methods = [m(1, 'run')];
    const entry: CapEntry = { plugId: 'p', generation: 0, version: v(), caps: { methods } };
    registry.register(entry);
    (methods[0]! as { name: string }).name = 'hacked';
    methods.push(m(2, 'extra'));
    const stored = registry.lookup('p');
    expect(stored?.caps.methods.map((method) => method.name)).toEqual(['run']);
    expect(Object.isFrozen(stored?.caps)).toBe(true);
    expect(Object.isFrozen(stored?.caps.methods)).toBe(true);
  });
});

describe('duplicate registration', () => {
  it('rejects a plugId already registered under any generation', () => {
    const registry = createCapabilityRegistry();
    const w = recorder('w');
    registry.watch(w.sink, 'w');
    registry.register(e('p', 0));
    for (const attempt of [e('p', 0), e('p', 1)]) {
      let thrown: unknown;
      try {
        registry.register(attempt);
      } catch (error) {
        thrown = error;
      }
      expect(statusOf(thrown)).toBe('duplicate');
    }
    expect(w.events.length).toBe(1);
    expect(registry.snapshot().length).toBe(1);
  });
});

describe('entry shape validation', () => {
  it('rejects malformed entries with invalid_argument without side effects', () => {
    const cases: readonly [string, unknown][] = [
      ['empty plugId', e('')],
      ['duplicate method id', e('p', 0, [m(1, 'a'), m(1, 'b')])],
      ['duplicate method name', e('p', 0, [m(1, 'a'), m(2, 'a')])],
      ['empty method name', e('p', 0, [m(1, '')])],
      ['negative method id', e('p', 0, [m(-3, 'a')])],
      ['fractional method id', e('p', 0, [m(1.5, 'a')])],
      ['negative generation', e('p', -1)],
      ['fractional generation', e('p', 1.5)],
      ['malformed version', e('p', 0, [m(1, 'a')], v(-1, 0, 0))],
      ['missing method set', { plugId: 'p', generation: 0, version: v(), caps: {} as CapsDesc }],
      ['non-JSON input schema', e('p', 0, [{ id: 1, name: 'a', inputSchema: Number.NaN }])],
    ];
    for (const [label, bad] of cases) {
      const registry = createCapabilityRegistry();
      const w = recorder('w');
      registry.watch(w.sink, 'w');
      let thrown: unknown;
      try {
        registry.register(bad as CapEntry);
      } catch (error) {
        thrown = error;
      }
      expect(thrown, label).toBeDefined();
      expect(statusOf(thrown), label).toBe('invalid_argument');
      expect(w.events.length, label).toBe(0);
      expect(registry.snapshot().length, label).toBe(0);
    }
  });
});

describe('withdraw', () => {
  it('withdraws an exact generation match and announces the empty method set', () => {
    const registry = createCapabilityRegistry();
    const w = recorder('w');
    registry.watch(w.sink, 'w');
    registry.register(e('p1', 2, [m(1, 'run')], v(3, 0, 1)));
    registry.withdraw('p1', 2);
    expect(facts(w.events)).toEqual([
      { plugId: 'p1', available: true },
      { plugId: 'p1', available: false },
    ]);
    const [gone] = w.events.slice(1);
    expect(gone?.version).toEqual({ major: 3, minor: 0, patch: 1 });
    expect(gone?.capabilities).toEqual({ methods: [] });
    expect(registry.snapshot()).toEqual([]);
    expect(registry.lookup('p1')).toBeUndefined();
  });

  it('ignores mismatched generations, unknown ids and repeated withdrawals', () => {
    const registry = createCapabilityRegistry();
    const w = recorder('w');
    registry.watch(w.sink, 'w');
    registry.register(e('p1', 2));
    expect(() => registry.withdraw('ghost', 0)).not.toThrowError();
    registry.withdraw('p1', 0);
    expect(w.events.length).toBe(1);
    expect(registry.lookup('p1')?.generation).toBe(2);
    registry.withdraw('p1', 2);
    registry.withdraw('p1', 2);
    expect(w.events.length).toBe(2);
    expect(registry.lookup('p1')).toBeUndefined();
  });

  it('accepts a fresh registration of the same plugId after withdrawal', () => {
    const registry = createCapabilityRegistry();
    const w = recorder('w');
    registry.watch(w.sink, 'w');
    registry.register(e('p', 0, [m(1, 'old')]));
    registry.withdraw('p', 0);
    registry.register(e('p', 1, [m(1, 'new')], v(9, 9, 9)));
    expect(registry.lookup('p')?.generation).toBe(1);
    expect(facts(w.events).map((fact) => fact.available)).toEqual([true, false, true]);
  });
});

describe('watch atomicity and ordering', () => {
  it('delivers the byte-ordered snapshot first, then changes in order', () => {
    const registry = createCapabilityRegistry();
    registry.register(e('b', 1));
    registry.register(e('a', 5));
    registry.register(e('A', 0));
    const w = recorder('w');
    const sub = registry.watch(w.sink, 'w');
    registry.register(e('c', 0));
    registry.withdraw('a', 5);
    expect(facts(w.events)).toEqual([
      { plugId: 'A', available: true },
      { plugId: 'a', available: true },
      { plugId: 'b', available: true },
      { plugId: 'c', available: true },
      { plugId: 'a', available: false },
    ]);
    expect(sub.snapshot.length).toBe(3);
    expect(sub.snapshot.every((event, index) => event === w.events[index])).toBe(true);
  });

  it('yields an empty snapshot for an empty registry and only live facts afterwards', () => {
    const registry = createCapabilityRegistry();
    const w = recorder('w');
    const sub = registry.watch(w.sink, 'w');
    expect(sub.snapshot).toEqual([]);
    expect(w.events.length).toBe(0);
    registry.register(e('p'));
    expect(w.events.length).toBe(1);
  });

  it('stops facts for a sink that unsubscribes inside its own callback', () => {
    const registry = createCapabilityRegistry();
    const events: CapEvent[] = [];
    let sub: { unsubscribe(): void } | undefined;
    const sink: CapSink = {
      onCapability(event: CapEvent): void {
        events.push(event);
        sub?.unsubscribe();
      },
    };
    sub = registry.watch(sink, 'self');
    registry.register(e('p1'));
    registry.register(e('p2'));
    expect(events.map((event) => event.plugId)).toEqual(['p1']);
  });
});

describe('sink failure isolation', () => {
  it('keeps other watchers and the table untouched when a sink throws', () => {
    const errors: [string, unknown][] = [];
    const registry = createCapabilityRegistry({
      onSinkError: (watcherId, error) => {
        errors.push([watcherId, error]);
      },
    });
    const bad = recorder('bad', 'throw');
    const good = recorder('good');
    registry.watch(bad.sink, 'bad');
    registry.watch(good.sink, 'good');
    registry.register(e('p1', 0));
    registry.register(e('p2', 0));
    expect(facts(good.events).map((fact) => fact.plugId)).toEqual(['p1', 'p2']);
    expect(bad.events.length).toBe(2);
    expect(errors.length).toBe(2);
    expect(errors[0]?.[0]).toBe('bad');
    expect((errors[0]?.[1] as Error).message).toBe('boom from bad');
    expect(registry.snapshot().length).toBe(2);
  });

  it('reports async rejections through onSinkError without blocking sync delivery', async () => {
    const errors: [string, unknown][] = [];
    const registry = createCapabilityRegistry({
      onSinkError: (watcherId, error) => {
        errors.push([watcherId, error]);
      },
    });
    const bad = recorder('bad', 'reject');
    const good = recorder('good');
    registry.watch(bad.sink, 'bad');
    registry.watch(good.sink, 'good');
    registry.register(e('p1', 0));
    expect(good.events.length).toBe(1);
    expect(errors.length).toBe(0);
    await flushMicrotasks();
    expect(errors.length).toBe(1);
    expect(errors[0]?.[0]).toBe('bad');
    expect((errors[0]?.[1] as Error).message).toBe('async boom from bad');
  });

  it('swallows sink failures without onSinkError and exceptions thrown by onSinkError', () => {
    const plain = createCapabilityRegistry();
    const bad1 = recorder('bad', 'throw');
    const good1 = recorder('good');
    plain.watch(bad1.sink, 'bad');
    plain.watch(good1.sink, 'good');
    expect(() => plain.register(e('p1', 0))).not.toThrowError();
    expect(good1.events.length).toBe(1);

    const loud = createCapabilityRegistry({ onSinkError: () => { throw new Error('reporter exploded'); } });
    const bad2 = recorder('bad2', 'throw');
    const good2 = recorder('good2');
    loud.watch(bad2.sink, 'bad2');
    loud.watch(good2.sink, 'good2');
    expect(() => loud.register(e('p2', 0))).not.toThrowError();
    expect(good2.events.length).toBe(1);
  });
});

describe('snapshot', () => {
  it('orders by plugId UTF-8 bytes rather than UTF-16 code units', () => {
    const registry = createCapabilityRegistry();
    const ids = ['\u{1F600}', 'b', '\uFFFD', 'A', 'a'];
    for (const id of ids) {
      registry.register(e(id));
    }
    // UTF-8: 'A' 0x41, 'a' 0x61, 'b' 0x62, U+FFFD 0xEF BF BD, emoji 0xF0 9F 98 80.
    expect(registry.snapshot().map((event) => event.plugId)).toEqual(['A', 'a', 'b', '\uFFFD', '\u{1F600}']);
    // Default string sort (UTF-16 code units) would place the emoji before U+FFFD.
    const defaultSort = [...ids].sort();
    expect(defaultSort[3]).toBe('\u{1F600}');
    expect(defaultSort[4]).toBe('\uFFFD');
  });

  it('returns a fresh array with fresh wrappers on every call', () => {
    const registry = createCapabilityRegistry();
    registry.register(e('p'));
    const first = registry.snapshot();
    const second = registry.snapshot();
    expect(second).not.toBe(first);
    expect(second[0]).not.toBe(first[0]);
    expect(second).toEqual(first);
    (first as CapEvent[]).pop();
    expect(registry.snapshot().length).toBe(1);
  });
});

describe('lookup', () => {
  it('returns the current entry or undefined', () => {
    const registry = createCapabilityRegistry();
    expect(registry.lookup('nope')).toBeUndefined();
    registry.register(e('p', 7, [m(1, 'run')], v(1, 2, 3)));
    const entry = registry.lookup('p');
    expect(entry?.plugId).toBe('p');
    expect(entry?.generation).toBe(7);
    expect(entry?.version).toEqual({ major: 1, minor: 2, patch: 3 });
    expect(entry?.caps.methods).toEqual([{ id: 1, name: 'run' }]);
    registry.withdraw('p', 7);
    expect(registry.lookup('p')).toBeUndefined();
  });
});
