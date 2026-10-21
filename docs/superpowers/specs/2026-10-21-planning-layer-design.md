# AsymSched Planning Layer Design

## Scope

Implement the paper's Decision/Planning phase as a pure, deterministic Python API. Measurement provides snapshots; Execution consumes plans. The planner performs neither sampling nor affinity/page migration. Preserve the existing `asymsched(apps, bandwidths, remote_access)` Demo endpoint as a documented compatibility entry point, not the authoritative multi-application planner.

## Contract

`plan_placement(bandwidths, apps, *, max_placements=100000)` accepts a square, nonnegative, finite, directed bandwidth matrix and a nonempty sequence of application snapshots. Each application has a unique string id, a nonempty list of distinct occupied node IDs (one slot per node), resident memory GB for each occupied node, a per-app source-node by destination-node matrix of nonnegative integer access bandwidths in bytes/second (the measurement layer aggregates and normalizes over the observation interval), and nonnegative finite `tm_seconds`/`tt_seconds`. Across applications, occupied nodes must be distinct. All inputs are snapshots; no mutation. The planner rejects invalid data with `ValueError` (or `TypeError` for wrong types), including more than `max_placements` candidate permutations before allocation. One-node and all-zero-traffic cases are valid. A source row represents access bandwidth observed for the thread group currently on that source node; measurement is responsible for attribution and aggregation.

`PlacementPlan` contains immutable snapshots of candidate and final node assignments by app, cluster members and weights, best weighted bandwidth, selected candidate score and migration GB, per-app current/candidate/final node tuples with corresponding slot order, approval/rejection with estimated seconds, and a global status (`unchanged`, `planned`, or `conflict`). It does not claim a migration happened. Node ordering is deterministic; the caller can serialize the plan without needing the original mutable input.

## Algorithm

For each app, node groups share a virtual cluster iff both have positive accesses to a common memory controller (matrix destination), transitively merging connected node groups. Isolated groups remain singleton clusters. For each cluster, sum the app-specific remote bandwidth from its source nodes to all other destination nodes; if the integer byte/second sum >0, weight is `log(sum)`, else weight is 0 (the paper leaves `log(0)` undefined). This choice of bytes/second fixes the logarithm's numerical scale; callers must not feed unnormalized counter values. All old occupied nodes are slots; generate distinct-node permutations in deterministic order, preserving each cluster's sorted slot order. Score with Demo's directed pairwise bandwidth sum per cluster, multiplied by its weight, then sum across clusters. This matches the exposed Demo `C_bw(P)` model, not an undocumented shared-link contention model.

Retain candidates with score >=0.9 * maximum score; choose minimum resident memory GB of changed slots, with lexicographically smallest tuple of target nodes as the tie break. Per app, a candidate move is approved if `tm_seconds + 0.3 * changed_slot_memory_gb <= 0.05 * tt_seconds`; an unchanged app is a no-op regardless of its historical tm. Reject an app's move if the inequality fails. Reverting such apps must not create duplicate occupied nodes; if it does, return an unchanged plan with conflict status rather than a misleading partial plan. Memory GB and times in seconds are explicit input units; 0.3 s/GB is the paper's experimental coefficient, not a Kunpeng measurement. Do not implement topology-specific pruning without a specified link criterion; cap permutations and fail loudly instead of returning a suboptimal partial search.

## Compatibility and testing

Retain Demo's existing JSON structure and function behavior; document its global remote matrix limitation, in-place model update, and absence of actual execution. Test validation, clustering, weighted asymmetric placement, zero traffic, 90% screening, ties, per-app migration rejection, conflict fallback, immutability, and cap. The old smoke tests must still pass. Do not run workloads or performance experiments for this phase.

## Version management

Work in nested `asymsched_demo` Git repository, keep original `origin` intact, add the requested fork as another named remote, verify branch ancestry, then commit and push without force. Preserve pre-existing local changes and identify them in commits if included.
