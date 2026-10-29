/**
 * @file Tests for the deterministic initialization planner.
 */
import { describe, expect, it } from 'vitest';
import type { OrderNodeInput } from '../src/core/types.ts';
import type { RackStatus } from '../src/core/status.ts';
import { isRackError } from '../src/core/status.ts';
import { comparePlugIds, createOrderPlanner, planOrder } from '../src/core/order.ts';

const planner = createOrderPlanner();

const set = (...ids: string[]): ReadonlySet<string> => new Set(ids);

const node = (
  plugId: string,
  priority = 0,
  before: readonly string[] = [],
  after: readonly string[] = [],
): OrderNodeInput => ({ plugId, priority, before, after });

const plannedIds = (nodes: readonly OrderNodeInput[], initialized: ReadonlySet<string> = set()): string[] =>
  planner.plan(nodes, initialized).map((index) => nodes[index]!.plugId);

interface CaughtRackError {
  status: RackStatus;
  detail: unknown;
}

const rackErrorOf = (run: () => unknown): CaughtRackError => {
  try {
    run();
  } catch (error) {
    if (!isRackError(error)) throw error;
    return { status: error.status, detail: error.detail };
  }
  throw new Error('expected plan() to throw a RackError');
};

describe('plugId byte-order comparison', () => {
  it('compares UTF-8 bytes instead of UTF-16 code units', () => {
    expect(comparePlugIds('a', 'a')).toBe(0);
    expect(comparePlugIds('a', 'ab')).toBeLessThan(0);
    expect(comparePlugIds('B', 'a')).toBeLessThan(0);
    // U+FFFF is EF BF BF; U+10000 is F0 90 80 80, so byte order puts U+FFFF first while
    // bare string comparison puts the surrogate pair first.
    expect('\u{10000}' < '\uFFFF').toBe(true);
    expect(comparePlugIds('\uFFFF', '\u{10000}')).toBeLessThan(0);
    expect(comparePlugIds('\u{10000}', '\uFFFF')).toBeGreaterThan(0);
  });
});

describe('order planner', () => {
  it('returns an empty plan for empty input', () => {
    expect(planner.plan([], set())).toEqual([]);
    expect(planner.plan([], set('com.example.base'))).toEqual([]);
  });

  it('orders ready plugins by ascending priority', () => {
    const nodes = [node('b', 10), node('a', 0), node('c', -3)];
    expect(plannedIds(nodes)).toEqual(['c', 'a', 'b']);
  });

  it('breaks priority ties by plugId UTF-8 byte order', () => {
    expect(plannedIds([node('c'), node('a'), node('b')])).toEqual(['a', 'b', 'c']);
    expect(plannedIds([node('a'), node('B')])).toEqual(['B', 'a']);
    const astral = '\u{10000}';
    const bmp = '\uFFFF';
    expect(plannedIds([node(astral), node(bmp)])).toEqual([bmp, astral]);
  });

  it('prefers hard edges over priority hints', () => {
    const nodes = [node('late', 100, ['early']), node('early', -100)];
    expect(plannedIds(nodes)).toEqual(['late', 'early']);
  });

  it('derives edges from before and after with mirrored semantics', () => {
    expect(plannedIds([node('a', 0, ['b']), node('b')])).toEqual(['a', 'b']);
    expect(plannedIds([node('a', 0, [], ['b']), node('b')])).toEqual(['b', 'a']);
  });

  it('deduplicates repeated edges so duplicates cannot fake a cycle', () => {
    const nodes = [node('a', 0, ['b', 'b']), node('b', 0, [], ['a', 'a'])];
    expect(plannedIds(nodes)).toEqual(['a', 'b']);
  });

  it('plans diamonds with a deterministic middle order', () => {
    const nodes = [
      node('end', 0, [], ['left', 'right']),
      node('right', 0, [], ['start']),
      node('start'),
      node('left', 0, [], ['start']),
    ];
    expect(plannedIds(nodes)).toEqual(['start', 'left', 'right', 'end']);
  });

  it('returns indices into the nodes array', () => {
    const nodes = [node('b'), node('c'), node('a')];
    const plan = planner.plan(nodes, set());
    expect(plan).toEqual([2, 0, 1]);
    for (const index of plan) expect(nodes[index]?.plugId).toBeDefined();
  });

  it('matches between the planner instance and the pure helper', () => {
    const nodes = [node('b', 1), node('a', 1)];
    expect(planOrder(nodes, set())).toEqual(planner.plan(nodes, set()));
  });
});

describe('order planner rejection rules', () => {
  it('rejects empty or non-string plug ids', () => {
    expect(rackErrorOf(() => planner.plan([node('')], set())).status).toBe('invalid_argument');
    const nonString = { plugId: 7, priority: 0, before: [], after: [] } as unknown as OrderNodeInput;
    expect(rackErrorOf(() => planner.plan([nonString], set())).status).toBe('invalid_argument');
  });

  it('rejects non-finite priorities', () => {
    expect(rackErrorOf(() => planner.plan([node('a', Number.NaN)], set())).status).toBe('invalid_argument');
    expect(rackErrorOf(() => planner.plan([node('a', Number.POSITIVE_INFINITY)], set())).status).toBe(
      'invalid_argument',
    );
  });

  it('rejects duplicate pending ids', () => {
    const caught = rackErrorOf(() => planner.plan([node('a'), node('b'), node('a')], set()));
    expect(caught.status).toBe('invalid_argument');
    expect((caught.detail as { plugId: string }).plugId).toBe('a');
  });

  it('rejects self references in before and after', () => {
    expect(rackErrorOf(() => planner.plan([node('a', 0, ['a'])], set())).status).toBe('invalid_argument');
    expect(rackErrorOf(() => planner.plan([node('a', 0, [], ['a'])], set())).status).toBe('invalid_argument');
  });

  it('rejects ids that are both pending and already initialized', () => {
    expect(rackErrorOf(() => planner.plan([node('a')], set('a'))).status).toBe('invalid_argument');
  });

  it('rejects unknown targets with not_found listing every target', () => {
    const caught = rackErrorOf(() =>
      planner.plan([node('b', 0, [], ['phantom']), node('a', 0, ['ghost'])], set()),
    );
    expect(caught.status).toBe('not_found');
    expect((caught.detail as { unknownTargets: string[] }).unknownTargets).toEqual(['ghost', 'phantom']);
  });

  it('reports the plug id sequence on a closed cycle', () => {
    const nodes = [node('a', 0, ['b']), node('b', 0, ['c']), node('c', 0, ['a'])];
    const caught = rackErrorOf(() => planner.plan(nodes, set()));
    expect(caught.status).toBe('cycle');
    const cycle = (caught.detail as { cycle: string[] }).cycle;
    expect(cycle).toEqual(['a', 'b', 'c']);
    const edges = new Set(['a->b', 'b->c', 'c->a']);
    for (let i = 0; i < cycle.length; i += 1) {
      const from = cycle[i]!;
      const to = cycle[(i + 1) % cycle.length]!;
      expect(edges.has(`${from}->${to}`)).toBe(true);
    }
  });

  it('reports the cycle itself, not merely blocked downstream nodes', () => {
    const nodes = [node('a', 0, ['b']), node('b', 0, ['a']), node('c', 0, [], ['a'])];
    const caught = rackErrorOf(() => planner.plan(nodes, set()));
    expect(caught.status).toBe('cycle');
    expect((caught.detail as { cycle: string[] }).cycle).toEqual(['a', 'b']);
  });

  it('prefers not_found over invalid_state when both apply', () => {
    const nodes = [node('a', 0, ['base']), node('b', 0, ['ghost'])];
    expect(rackErrorOf(() => planner.plan(nodes, set('base'))).status).toBe('not_found');
  });
});

describe('order planner hot-load rules', () => {
  it('satisfies after constraints from already initialized ids without an edge', () => {
    expect(plannedIds([node('p', 0, [], ['base'])], set('base'))).toEqual(['p']);
    // p keeps no edge to the initialized 'base', so q wins purely by priority.
    const nodes = [node('p', 5, [], ['base']), node('q', 1)];
    expect(plannedIds(nodes, set('base'))).toEqual(['q', 'p']);
  });

  it('rejects before constraints against already initialized ids', () => {
    const nodes = [node('p', 0, ['base'])];
    const caught = rackErrorOf(() => planner.plan(nodes, set('base')));
    expect(caught.status).toBe('invalid_state');
    expect((caught.detail as { conflicts: { plugId: string; target: string }[] }).conflicts).toEqual([
      { plugId: 'p', target: 'base' },
    ]);
  });
});

describe('order planner determinism', () => {
  it('produces the same plan regardless of input order', () => {
    // Edges: power->net, storage->index, net->index, index->search.
    const expected = ['power', 'storage', 'net', 'index', 'search'];
    const permutations: OrderNodeInput[][] = [
      [
        node('search', 5, [], ['index']),
        node('storage'),
        node('index', 5, [], ['net', 'storage']),
        node('power'),
        node('net', 10, [], ['power']),
      ],
      [
        node('index', 5, [], ['net', 'storage']),
        node('power'),
        node('search', 5, [], ['index']),
        node('net', 10, [], ['power']),
        node('storage'),
      ],
      [
        node('net', 10, [], ['power']),
        node('search', 5, [], ['index']),
        node('storage'),
        node('index', 5, [], ['net', 'storage']),
        node('power'),
      ],
    ];
    for (const nodes of permutations) {
      const plan = planner.plan(nodes, set());
      expect(plan.map((index) => nodes[index]!.plugId)).toEqual(expected);
      expect([...plan].sort((left, right) => left - right)).toEqual([0, 1, 2, 3, 4]);
    }
  });
});
