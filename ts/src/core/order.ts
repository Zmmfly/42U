/**
 * @file Deterministic initialization planning for pending plugins.
 *
 * @brief Topological planner (TS edition of the C++ order.cc): validate identities, derive hard
 *        before/after edges, apply the hot-load rules and emit a deterministic plan.
 *
 * @note The planner is a pure function over immutable node descriptions: it reads and mutates no
 *       host state. Ordering rules: hard before/after edges first; among currently ready nodes
 *       ascending priority, then plugId UTF-8 byte order, so a plan never depends on the order in
 *       which plugins were enumerated.
 */

import { Buffer } from 'node:buffer';

import type { OrderNodeInput, OrderPlanner } from './types.ts';
import { RackError } from './status.ts';

/**
 * @brief Compare two plug ids by their UTF-8 bytes interpreted as unsigned values.
 *
 * @param left First plug id.
 * @param right Second plug id.
 * @return A negative value when left sorts first, zero when equal, positive otherwise.
 * @note Bare `<` on strings orders UTF-16 code units, which disagrees with byte order for
 *       astral characters (U+FFFF sorts after U+10000 by code units but before it by UTF-8
 *       bytes). The rack promises byte order, matching the C++ unsigned-char comparison.
 */
export function comparePlugIds(left: string, right: string): number {
  return Buffer.compare(Buffer.from(left, 'utf8'), Buffer.from(right, 'utf8'));
}

/** @brief Whether ready node a must be planned before ready node b (priority, then bytes). */
function comesBefore(nodes: readonly OrderNodeInput[], a: number, b: number): boolean {
  const left = nodes[a]!;
  const right = nodes[b]!;
  return left.priority !== right.priority
    ? left.priority < right.priority
    : comparePlugIds(left.plugId, right.plugId) < 0;
}

/**
 * @brief Assert well-formed pending identities and their separation from initialized ids.
 *
 * @param nodes Pending plugins as given.
 * @param initializedIds Ids the host reports as already initialized.
 * @return plugId to index map for edge construction.
 * @throws RackError 'invalid_argument' for an empty or non-string plugId, a non-finite priority,
 *         a duplicate pending plugId, a self reference in before/after, or a plugId that is
 *         both pending and already initialized.
 */
function validateIdentities(
  nodes: readonly OrderNodeInput[],
  initializedIds: ReadonlySet<string>,
): Map<string, number> {
  const indexByPlugId = new Map<string, number>();
  for (const [index, node] of nodes.entries()) {
    if (typeof node.plugId !== 'string' || node.plugId.length === 0) {
      throw new RackError('invalid_argument', `pending node ${index} has an empty or non-string plugId`, {
        index,
        field: 'plugId',
      });
    }
    if (!Number.isFinite(node.priority)) {
      throw new RackError('invalid_argument', `plugin '${node.plugId}' has a non-finite priority`, {
        index,
        plugId: node.plugId,
        field: 'priority',
      });
    }
    const first = indexByPlugId.get(node.plugId);
    if (first !== undefined) {
      throw new RackError('invalid_argument', `duplicate pending plugId '${node.plugId}'`, {
        plugId: node.plugId,
        indices: [first, index],
      });
    }
    indexByPlugId.set(node.plugId, index);
  }
  for (const plugId of indexByPlugId.keys()) {
    if (initializedIds.has(plugId)) {
      throw new RackError('invalid_argument', `plugId '${plugId}' is both pending and already initialized`, {
        plugId,
      });
    }
  }
  for (const node of nodes) {
    for (const field of ['before', 'after'] as const) {
      if (node[field].includes(node.plugId)) {
        throw new RackError('invalid_argument', `plugin '${node.plugId}' lists itself in ${field}`, {
          plugId: node.plugId,
          field,
        });
      }
    }
  }
  return indexByPlugId;
}

/** @brief Deduplicated hard-edge graph over pending node indices. */
interface ConstraintGraph {
  readonly successors: number[][];
  readonly indegree: number[];
}

/**
 * @brief Derive hard edges from before/after declarations and enforce the hot-load rules.
 *
 * @param nodes Pending plugins as given.
 * @param initializedIds Ids already initialized (hot-load context).
 * @param indexByPlugId plugId to index map from validateIdentities.
 * @return Successor lists and indegrees with duplicate edges collapsed.
 * @throws RackError 'not_found' when a before/after target is neither pending nor initialized;
 *         detail.unknownTargets lists every such target in byte order.
 * @throws RackError 'invalid_state' when a `before` target is already initialized (the new
 *         plugin cannot move in front of it); detail.conflicts lists every violation.
 * @note Hot-load asymmetry: an `after` target in initializedIds is already satisfied and adds
 *       no edge. Unknown targets are reported before state conflicts so the caller always sees
 *       the complete graph problem first.
 */
function collectEdges(
  nodes: readonly OrderNodeInput[],
  initializedIds: ReadonlySet<string>,
  indexByPlugId: Map<string, number>,
): ConstraintGraph {
  const successors: number[][] = Array.from({ length: nodes.length }, (): number[] => []);
  const indegree: number[] = Array.from({ length: nodes.length }, (): number => 0);
  const edgeKeys = new Set<string>();
  const unknownTargets = new Set<string>();
  const conflicts: { plugId: string; target: string }[] = [];

  // A hard edge may be declared from either endpoint; keep exactly one copy so repeated
  // declarations cannot inflate indegrees and fake a cycle.
  const addEdge = (from: number, to: number): void => {
    const key = `${from}:${to}`;
    if (edgeKeys.has(key)) return;
    edgeKeys.add(key);
    successors[from]?.push(to);
    indegree[to] = (indegree[to] ?? 0) + 1;
  };

  for (const [index, node] of nodes.entries()) {
    for (const target of node.before) {
      if (initializedIds.has(target)) {
        conflicts.push({ plugId: node.plugId, target });
        continue;
      }
      const targetIndex = indexByPlugId.get(target);
      if (targetIndex === undefined) {
        unknownTargets.add(target);
        continue;
      }
      addEdge(index, targetIndex);
    }
    for (const target of node.after) {
      if (initializedIds.has(target)) continue;
      const targetIndex = indexByPlugId.get(target);
      if (targetIndex === undefined) {
        unknownTargets.add(target);
        continue;
      }
      addEdge(targetIndex, index);
    }
  }

  if (unknownTargets.size > 0) {
    const list = [...unknownTargets].sort(comparePlugIds);
    const rendered = list.map((id) => `'${id}'`).join(', ');
    throw new RackError('not_found', `unknown before/after constraint targets: ${rendered}`, {
      unknownTargets: list,
    });
  }
  if (conflicts.length > 0) {
    conflicts.sort((left, right) => comparePlugIds(left.plugId, right.plugId));
    const rendered = conflicts.map((c) => `'${c.plugId}' before '${c.target}'`).join(', ');
    throw new RackError('invalid_state', `cannot initialize before already initialized plugins: ${rendered}`, {
      conflicts,
    });
  }
  return { successors, indegree };
}

/**
 * @brief Extract one deterministic cycle from the unplannable remainder of the graph.
 *
 * @param nodes Pending plugins as given.
 * @param graph Graph whose Kahn run has finished; a positive indegree marks an unplannable node.
 * @return The plug ids along a closed walk of the cycle, in edge direction.
 * @note Every unplannable node keeps at least one unplannable successor (its edge was never
 *       consumed), so following the smallest-id successor from the smallest unplannable id must
 *       revisit a node; the walk segment back to that node is a simple cycle.
 */
function extractCycle(nodes: readonly OrderNodeInput[], graph: ConstraintGraph): string[] {
  let start = -1;
  for (const [index] of nodes.entries()) {
    if ((graph.indegree[index] ?? 0) === 0) continue;
    if (start < 0 || comparePlugIds(nodes[index]!.plugId, nodes[start]!.plugId) < 0) start = index;
  }
  const position = new Map<number, number>();
  const walk: number[] = [];
  let current = start;
  while (current >= 0 && !position.has(current)) {
    position.set(current, walk.length);
    walk.push(current);
    let next = -1;
    for (const candidate of graph.successors[current] ?? []) {
      if ((graph.indegree[candidate] ?? 0) === 0) continue;
      if (next < 0 || comparePlugIds(nodes[candidate]!.plugId, nodes[next]!.plugId) < 0) next = candidate;
    }
    current = next;
  }
  const from = current >= 0 ? (position.get(current) ?? 0) : 0;
  return walk.slice(from).map((index) => nodes[index]!.plugId);
}

/**
 * @brief Kahn topological sort with deterministic ready selection.
 *
 * @param nodes Pending plugins as given.
 * @param graph Deduplicated hard-edge graph.
 * @return Indices into nodes in plan order.
 * @throws RackError 'cycle' when nodes remain unplannable; detail.cycle holds the plug ids on
 *         the cycle in edge direction.
 */
function schedule(nodes: readonly OrderNodeInput[], graph: ConstraintGraph): number[] {
  const ready: number[] = [];
  for (const [index] of nodes.entries()) {
    if ((graph.indegree[index] ?? 0) === 0) ready.push(index);
  }

  const plan: number[] = [];
  while (ready.length > 0) {
    // Among ready nodes pick ascending priority, then plugId UTF-8 byte order.
    let best = 0;
    for (let candidate = 1; candidate < ready.length; candidate += 1) {
      if (comesBefore(nodes, ready[candidate]!, ready[best]!)) best = candidate;
    }
    const index = ready[best] ?? 0;
    ready.splice(best, 1);
    plan.push(index);
    for (const next of graph.successors[index] ?? []) {
      const remaining = (graph.indegree[next] ?? 0) - 1;
      graph.indegree[next] = remaining;
      if (remaining === 0) ready.push(next);
    }
  }

  if (plan.length !== nodes.length) {
    const cycle = extractCycle(nodes, graph);
    const rendered = [...cycle, cycle[0] ?? ''].join(' -> ');
    throw new RackError('cycle', `initialization constraints form a cycle: ${rendered}`, { cycle });
  }
  return plan;
}

/**
 * @brief Plan the initialization order for pending plugins (pure function).
 *
 * @param nodes Pending plugins with their hard constraints; returned indices refer to this array.
 * @param initializedIds Ids already initialized (hot-load): `after` may be satisfied by them,
 *        `before` against them is impossible and is rejected.
 * @return Indices into nodes in initialization order; empty input yields an empty plan. The
 *         mapped plugId sequence is identical for any permutation of the input.
 *
 * @throws RackError 'invalid_argument' for empty or duplicate pending ids, non-finite
 *         priorities, self references, or ids that are both pending and initialized.
 * @throws RackError 'not_found' for before/after targets that are neither pending nor
 *         initialized; detail.unknownTargets lists them all.
 * @throws RackError 'invalid_state' when a `before` target is already initialized;
 *         detail.conflicts lists every violation.
 * @throws RackError 'cycle' when constraints form a cycle; detail.cycle holds the plug ids on
 *         the cycle in edge direction.
 * @note Statuses follow the frozen TS contract, which maps duplicate ids and self references to
 *       'invalid_argument'; the C++ ABI used its separate 'duplicate' and 'cycle' codes there.
 */
export function planOrder(
  nodes: readonly OrderNodeInput[],
  initializedIds: ReadonlySet<string>,
): number[] {
  const indexByPlugId = validateIdentities(nodes, initializedIds);
  const graph = collectEdges(nodes, initializedIds, indexByPlugId);
  return schedule(nodes, graph);
}

/**
 * @brief Create the deterministic initialization planner.
 *
 * @return A stateless OrderPlanner whose plan() is a pure function of its arguments.
 * @note One instance can serve any number of independent planning requests.
 */
export function createOrderPlanner(): OrderPlanner {
  return { plan: planOrder };
}
