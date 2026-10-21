# AsymSched Planning Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a complete, pure first-stage planning API over attributed measurement snapshots.

**Architecture:** Introduce typed immutable snapshots/results in `asymsched/planning.py`; retain the old Demo entry point. The planner validates inputs, groups node slots, ranks legal placements, checks per-app migration costs and returns a conflict-free plan without touching the operating system.

**Tech Stack:** Python 3 standard library, unittest.

**Spec:** `docs/superpowers/specs/2026-10-21-planning-layer-design.md`

## Global Constraints

- No performance experiments, sampling, thread binding or page migration.
- New input memory is GB; time is seconds; accesses are integer byte/second rates attributed per app/source/controller.
- Keep the original Demo call and JSON behavior available.
- Preserve existing local modifications and never force-push the fork.

---

### Task 1: Input, output and clustering

**Files:** Create `asymsched/planning.py`, `tests/test_planning.py`.

**Interfaces:** `AppSnapshot(app_id, current_nodes, memory_gb, accesses, tm_seconds, tt_seconds)`; `plan_placement(bandwidths, apps, *, max_placements=100000) -> PlacementPlan`.

- [ ] **Step 1: Test validation, per-app attribution, transitive grouping and immutable snapshots.**

```python
with self.assertRaises(ValueError):
    plan_placement([[0]], [AppSnapshot('x', (0,), (1,), ((-1,),), 0, 1)])
a = AppSnapshot('x', (0, 1), (1, 1), ((0, 4), (0, 2)), 0, 100)
self.assertEqual(plan_placement([[0, 5], [5, 0]], [a]).clusters[0].nodes, (0, 1))
```

- [ ] **Step 2: Run `python3 -m unittest tests.test_planning -v`; verify missing module/behavior fails.**
- [ ] **Step 3: Implement frozen dataclasses, validation and same-app common-controller grouping; determine cluster weight from attributed remote counts.**
- [ ] **Step 4: Run the same test until green and commit this independently testable behavior.**

### Task 2: Placement scoring, selection and migration decision

**Files:** Modify `asymsched/planning.py`, `tests/test_planning.py`.

**Interfaces:** `PlacementPlan.candidate`, `.final`, `.clusters`, `.decisions`, `.best_score`, `.score`, `.migration_gb`, `.status`.

- [ ] **Step 1: Test non-symmetric bandwidth, 90% threshold, minimal migration tie, rejected application and conflict fallback.**

```python
self.assertEqual(plan_placement([[0, 5], [9, 0]], [a]).best_score,
                 plan_placement([[0, 5], [9, 0]], [a]).score)
self.assertEqual(plan_placement([[0]], [AppSnapshot('x', (0,), (1,), ((0,),), 0, 0)]).status,
                 'unchanged')
```

- [ ] **Step 2: Run `python3 -m unittest tests.test_planning -v`; verify intended failures.**
- [ ] **Step 3: Implement permutation scoring, candidate filtering, deterministic minimum migration, per-app decisions, conflict handling and permutation cap.**
- [ ] **Step 4: Run tests and compile `python3 -m compileall -q asymsched`; commit planner logic.**

### Task 3: Document and sync compatibility

**Files:** Modify `README.md`; retain `asymsched/asymsched.py`, `asymsched/rest_server.py`, `asymsched/kunpeng_adapter.py`, `tests/test_asymsched.py` as existing Demo work.

**Interfaces:** README specifies exact planner types/units and distinction from the legacy mutating Demo endpoint.

- [ ] **Step 1: Include documented minimal call example.**

```python
from asymsched.planning import AppSnapshot, plan_placement
snapshot = AppSnapshot('a', (0,), (1.0,), ((0,),), 0.0, 10.0)
plan = plan_placement(((0,),), (snapshot,))
assert plan.status == 'unchanged'
```

- [ ] **Step 2: Verify `python3 -m unittest discover -s tests -v`, `python3 -m compileall -q asymsched`, `git diff --check`, and review commit scope.**
- [ ] **Step 3: Compare fork master tip, add named `openeuler` remote, commit related work and push `master` without force.**

```bash
git remote add openeuler https://github.com/IpartmentXHC/asymsched_openeuler.git
git fetch openeuler master
git merge-base --is-ancestor openeuler/master HEAD
git push openeuler master
```
