"""Pure node-level AsymSched planning over attributed measurement snapshots.

The caller measures accesses and resident memory; this module never executes a plan.
"""

from dataclasses import dataclass
from itertools import permutations
from math import isclose, isfinite, log, perm
from numbers import Integral, Real


@dataclass(frozen=True)
class AppSnapshot:
    app_id: str
    current_nodes: tuple[int, ...]
    memory_gb: tuple[float, ...]
    accesses: tuple[tuple[int, ...], ...]
    tm_seconds: float
    tt_seconds: float

    def __post_init__(self):
        object.__setattr__(self, 'current_nodes', tuple(self.current_nodes))
        object.__setattr__(self, 'memory_gb', tuple(self.memory_gb))
        object.__setattr__(self, 'accesses', tuple(tuple(row) for row in self.accesses))


@dataclass(frozen=True)
class ClusterPlan:
    app_id: str
    nodes: tuple[int, ...]
    remote_bandwidth_bps: int
    weight: float


@dataclass(frozen=True)
class AppDecision:
    app_id: str
    current_nodes: tuple[int, ...]
    candidate_nodes: tuple[int, ...]
    final_nodes: tuple[int, ...]
    migration_gb: float
    estimated_seconds: float
    approved: bool
    reason: str


@dataclass(frozen=True)
class PlacementPlan:
    candidate: tuple[tuple[int, ...], ...]
    final: tuple[tuple[int, ...], ...]
    clusters: tuple[ClusterPlan, ...]
    decisions: tuple[AppDecision, ...]
    best_score: float
    score: float
    migration_gb: float
    qualifying_count: int
    status: str


def _nonnegative_number(value, name):
    if isinstance(value, bool) or not isinstance(value, Real) or not isfinite(value) or value < 0:
        raise ValueError(f'{name} must be a finite nonnegative number')


def _matrix(matrix, size, name, integer=False):
    if len(matrix) != size or any(len(row) != size for row in matrix):
        raise ValueError(f'{name} must be a square {size}x{size} matrix')
    for row in matrix:
        for value in row:
            if integer and (isinstance(value, bool) or not isinstance(value, Integral)):
                raise ValueError(f'{name} must contain nonnegative integer counts')
            _nonnegative_number(value, name)
    return tuple(tuple(row) for row in matrix)


def _validate(bandwidths, apps, max_placements):
    if isinstance(max_placements, bool) or not isinstance(max_placements, Integral) or max_placements < 1:
        raise ValueError('max_placements must be a positive integer')
    size = len(bandwidths)
    if size == 0:
        raise ValueError('at least one NUMA node is required')
    bandwidths = _matrix(bandwidths, size, 'bandwidths')
    apps = tuple(apps)
    if not apps:
        raise ValueError('at least one app is required')
    occupied = set()
    names = set()
    for app in apps:
        if not isinstance(app, AppSnapshot):
            raise TypeError('apps must contain AppSnapshot values')
        if not isinstance(app.app_id, str) or not app.app_id or app.app_id in names:
            raise ValueError('app ids must be distinct nonempty strings')
        names.add(app.app_id)
        nodes = app.current_nodes
        if not nodes or len(nodes) != len(app.memory_gb):
            raise ValueError('each app needs one memory_gb value per occupied node')
        for node in nodes:
            if isinstance(node, bool) or not isinstance(node, Integral) or node < 0 or node >= size:
                raise ValueError('occupied node is outside the topology')
            if node in occupied:
                raise ValueError('occupied nodes must be distinct across all apps')
            occupied.add(node)
        for amount in app.memory_gb:
            _nonnegative_number(amount, 'memory_gb')
        _nonnegative_number(app.tm_seconds, 'tm_seconds')
        _nonnegative_number(app.tt_seconds, 'tt_seconds')
        _matrix(app.accesses, size, 'accesses', integer=True)
    if perm(size, len(occupied)) > max_placements:
        raise ValueError('placement count exceeds max_placements; no partial search performed')
    return bandwidths, apps


def _clusters(app):
    nodes = app.current_nodes
    parent = {node: node for node in nodes}

    def root(node):
        while parent[node] != node:
            node = parent[node]
        return node

    for index, left in enumerate(nodes):
        for right in nodes[index + 1:]:
            if any(app.accesses[left][dest] > 0 and app.accesses[right][dest] > 0
                   for dest in range(len(app.accesses))):
                parent[root(right)] = root(left)
    groups = {}
    for node in nodes:
        groups.setdefault(root(node), []).append(node)
    result = []
    for members in sorted(groups.values(), key=lambda group: min(group)):
        count = sum(app.accesses[source][dest] for source in members
                    for dest in range(len(app.accesses)) if dest != source)
        result.append(ClusterPlan(app.app_id, tuple(sorted(members)), count,
                                  log(count) if count else 0.0))
    return tuple(result)


def _score(target, clusters, bandwidths, slot_indexes):
    score = 0.0
    for cluster, indexes in zip(clusters, slot_indexes):
        for src in indexes:
            for dst in indexes:
                if src != dst:
                    score += bandwidths[target[src]][target[dst]] * cluster.weight
    return score


def plan_placement(bandwidths, apps, *, max_placements=100000):
    """Return a deterministic, conflict-free plan; never modify apps or execute moves.

    `memory_gb` is GB, `tm_seconds` and `tt_seconds` are seconds, and
    `accesses` are per-app integer bytes/second rates indexed by source and
    memory controller; measurement must normalize raw counters before calling.
    """
    bandwidths, apps = _validate(bandwidths, apps, max_placements)
    clusters = tuple(cluster for app in apps for cluster in _clusters(app))
    current = tuple(tuple(app.current_nodes) for app in apps)
    slots = tuple(node for app in apps for node in app.current_nodes)
    memories = tuple(amount for app in apps for amount in app.memory_gb)
    indexes = {node: index for index, node in enumerate(slots)}
    cluster_indexes = tuple(tuple(indexes[node] for node in cluster.nodes)
                            for cluster in clusters)
    candidates = []
    for target in permutations(range(len(bandwidths)), len(slots)):
        score = _score(target, clusters, bandwidths, cluster_indexes)
        moved = sum(amount for index, amount in enumerate(memories)
                    if slots[index] != target[index])
        if not isfinite(score) or not isfinite(moved):
            raise ValueError('placement score or migration amount is not representable')
        candidates.append((score, moved, target))
    best_score = max(row[0] for row in candidates)
    threshold = 0.9 * best_score
    qualifying = [row for row in candidates
                  if row[0] >= threshold or isclose(row[0], threshold, rel_tol=1e-12, abs_tol=1e-12)]
    score, migration_gb, target = min(qualifying, key=lambda row: (row[1], row[2]))
    proposed = []
    offset = 0
    for app in apps:
        count = len(app.current_nodes)
        proposed.append(tuple(target[offset:offset + count]))
        offset += count
    candidate = tuple(proposed)

    decisions = []
    for app, original, destination in zip(apps, current, candidate):
        moved = sum(amount for old, new, amount in zip(original, destination, app.memory_gb)
                    if old != new)
        changed = original != destination
        estimated = app.tm_seconds + 0.3 * moved if changed else app.tm_seconds
        if not isfinite(estimated):
            raise ValueError('estimated migration time is not representable')
        approved = not changed or estimated <= 0.05 * app.tt_seconds
        final_nodes = destination if approved else original
        reason = 'no_change' if not changed else ('approved' if approved else 'migration_budget')
        decisions.append(AppDecision(app.app_id, original, destination, final_nodes, moved,
                                     estimated, approved, reason))
    final = tuple(decision.final_nodes for decision in decisions)
    flattened = tuple(node for nodes in final for node in nodes)
    if len(set(flattened)) != len(flattened):
        final = current
        decisions = [AppDecision(d.app_id, original, d.candidate_nodes, original,
                                 d.migration_gb, d.estimated_seconds, False,
                                 'node_conflict') for d, original in zip(decisions, current)]
        status = 'conflict'
    else:
        status = 'planned' if final != current else 'unchanged'
    return PlacementPlan(candidate, final, clusters, tuple(decisions), best_score,
                         score, migration_gb, len(qualifying), status)
