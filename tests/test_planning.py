import dataclasses
import math
import unittest

from asymsched.planning import AppSnapshot, plan_placement


def app(name, nodes, memory, accesses, tm=0.0, tt=100.0):
    return AppSnapshot(name, tuple(nodes), tuple(memory), tuple(map(tuple, accesses)), tm, tt)


class PlanningTests(unittest.TestCase):
    def test_common_controller_clusters_same_app_transitively(self):
        accesses = ((0, 0, 0, 2), (0, 0, 3, 4), (0, 0, 5, 0), (0, 0, 0, 0))
        result = plan_placement([[0] * 4 for _ in range(4)],
                                [app('a', (0, 1, 2), (1, 1, 1), accesses)])
        self.assertEqual(result.clusters[0].nodes, (0, 1, 2))
        self.assertEqual(result.clusters[0].remote_bandwidth_bps, 9)
        self.assertAlmostEqual(result.clusters[0].weight, math.log(9))

    def test_app_attribution_does_not_cluster_across_apps(self):
        matrix = ((0, 0, 5), (0, 0, 0), (0, 0, 0))
        result = plan_placement([[0] * 3 for _ in range(3)],
                                [app('a', (0,), (1,), matrix),
                                 app('b', (1,), (1,), matrix)])
        self.assertEqual([(c.app_id, c.nodes) for c in result.clusters],
                         [('a', (0,)), ('b', (1,))])
        self.assertEqual([c.remote_bandwidth_bps for c in result.clusters], [5, 0])

    def test_asymmetric_bandwidth_drives_ordered_slots(self):
        bandwidths = ((0, 1, 1), (1, 0, 20), (1, 2, 0))
        accesses = ((0, 0, 4), (0, 0, 3), (0, 0, 0))
        result = plan_placement(bandwidths, [app('a', (0, 1), (2, 1), accesses)])
        self.assertEqual(result.candidate, ((2, 1),))
        self.assertAlmostEqual(result.best_score, 22 * math.log(7))
        self.assertEqual(result.migration_gb, 2)
        self.assertEqual(result.status, 'planned')

    def test_minimum_memory_wins_among_near_optimal_candidates(self):
        bandwidths = ((0, 10, 0), (10, 0, 9), (0, 9, 0))
        accesses = ((0, 2, 0), (0, 0, 0), (0, 2, 0))
        result = plan_placement(bandwidths, [app('a', (0, 2), (10, 1), accesses)])
        self.assertEqual(result.candidate, ((0, 1),))
        self.assertEqual(result.migration_gb, 1)
        self.assertGreater(result.qualifying_count, 1)

    def test_placement_below_ninety_percent_is_excluded(self):
        bandwidths = ((0, 10, 8), (10, 0, 0), (8, 0, 0))
        accesses = ((0, 4, 0), (0, 0, 0), (0, 4, 0))
        result = plan_placement(bandwidths, [app('a', (0, 2), (1, 1), accesses)])
        self.assertEqual(result.candidate, ((0, 1),))
        self.assertAlmostEqual(result.score, result.best_score)
        bandwidths = ((0, 10, 9), (10, 0, 0), (9, 0, 0))
        boundary = plan_placement(bandwidths, [app('a', (0, 2), (1, 1), accesses)])
        self.assertEqual(boundary.candidate, ((0, 2),))

    def test_one_app_can_be_rejected_without_blocking_another(self):
        bandwidths = tuple(tuple(20 if {src, dst} in ({2, 3}, {6, 7}) and src != dst
                                     else (0 if src == dst else 1)
                                     for dst in range(8)) for src in range(8))
        access_a = tuple(tuple(3 if src in (0, 1) and dst == 7 else 0
                               for dst in range(8)) for src in range(8))
        access_b = tuple(tuple(3 if src in (4, 5) and dst == 7 else 0
                               for dst in range(8)) for src in range(8))
        result = plan_placement(bandwidths, [
            app('a', (0, 1), (1, 1), access_a, tm=100),
            app('b', (4, 5), (1, 1), access_b),
        ])
        self.assertEqual(result.decisions[0].reason, 'migration_budget')
        self.assertEqual(result.final[0], (0, 1))
        self.assertEqual(result.final[1], (6, 7))
        self.assertEqual(result.status, 'planned')

    def test_zero_access_and_single_node_is_unchanged(self):
        result = plan_placement(((0,),), [app('a', (0,), (1,), ((0,),), tt=0)])
        self.assertEqual(result.status, 'unchanged')
        self.assertEqual(result.clusters[0].weight, 0)
        self.assertEqual(result.candidate, ((0,),))
        self.assertEqual(result.final, ((0,),))

    def test_per_app_budget_vetoes_movement(self):
        bandwidths = ((0, 1, 1), (1, 0, 20), (1, 2, 0))
        accesses = ((0, 0, 4), (0, 0, 4), (0, 0, 0))
        result = plan_placement(bandwidths, [app('a', (0, 1), (2, 1), accesses, tm=5)])
        self.assertEqual(result.candidate, ((2, 1),))
        self.assertEqual(result.final, ((0, 1),))
        self.assertEqual(result.status, 'unchanged')
        self.assertEqual(result.decisions[0].reason, 'migration_budget')
        self.assertAlmostEqual(result.decisions[0].estimated_seconds, 5.6)

    def test_budget_equality_allows_movement(self):
        bandwidths = ((0, 1, 1), (1, 0, 20), (1, 2, 0))
        accesses = ((0, 0, 4), (0, 0, 4), (0, 0, 0))
        result = plan_placement(bandwidths, [app('a', (0, 1), (2, 1), accesses, tt=12)])
        self.assertEqual(result.status, 'planned')
        self.assertTrue(result.decisions[0].approved)

    def test_conflict_after_per_app_veto_returns_global_noop(self):
        bandwidths = ((0, 1, 20), (1, 0, 1), (20, 1, 0))
        accesses = ((0, 0, 0), (0, 0, 5), (0, 0, 4))
        a = app('a', (0,), (1,), accesses)
        b = app('b', (1, 2), (1, 1), accesses, tm=100)
        result = plan_placement(bandwidths, [a, b])
        self.assertEqual(result.final, ((0,), (1, 2)))
        self.assertEqual(result.status, 'conflict')

    def test_serialized_plan_retains_original_slot_order(self):
        accesses = ((0, 0, 2), (0, 0, 0), (0, 0, 2))
        result = plan_placement(((0, 1, 1), (1, 0, 1), (1, 1, 0)),
                                [app('a', (2, 0), (1, 2), accesses)])
        self.assertEqual(result.decisions[0].current_nodes, (2, 0))
        self.assertEqual(result.clusters[0].nodes, (0, 2))
        self.assertEqual(result.decisions[0].candidate_nodes, (2, 0))

    def test_input_and_output_are_immutable_snapshots(self):
        raw = [[0, 2], [3, 0]]
        accesses = [[0, 2], [2, 0]]
        sample = app('a', (0, 1), (1, 1), accesses)
        result = plan_placement(raw, [sample])
        raw[0][1] = 999
        accesses[0][1] = 999
        self.assertEqual(sum(cluster.remote_bandwidth_bps for cluster in result.clusters), 4)
        with self.assertRaises(dataclasses.FrozenInstanceError):
            result.status = 'changed'
        with self.assertRaises(dataclasses.FrozenInstanceError):
            sample.tt_seconds = 0

    def test_unrepresentable_score_fails_instead_of_returning_infinity(self):
        accesses = ((4, 0), (4, 0))
        with self.assertRaises(ValueError):
            plan_placement(((0, 1e308), (1e308, 0)),
                           [app('a', (0, 1), (1, 1), accesses)])

    def test_invalid_contract_and_search_budget(self):
        base = app('a', (0,), (1,), ((0,),))
        invalid = [
            ((), (base,)),
            (((0, 2),), (base,)),
            (((0,),), (app('a', (0,), (1,), ((-1,),)),)),
            (((0,),), (app('a', (0,), (1,), ((0,),), tt=-1),)),
            (((0,),), (base, base)),
            (((0,),), (app('a', (1,), (1,), ((0,),)),)),
            (((0,),), (app('a', (0, 0), (1, 1), ((0,),)),)),
            (((0,),), (app('a', (0,), (float('nan'),), ((0,),)),)),
        ]
        for bandwidths, apps in invalid:
            with self.subTest(bandwidths=bandwidths, apps=apps):
                with self.assertRaises((ValueError, TypeError)):
                    plan_placement(bandwidths, apps)
        with self.assertRaises(ValueError):
            plan_placement(((0, 1), (1, 0)), (base,), max_placements=1)


if __name__ == '__main__':
    unittest.main()
