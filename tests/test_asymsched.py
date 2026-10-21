import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1]))

from asymsched.asymsched import asymsched
from asymsched.models import App, Cluster
from asymsched.kunpeng_adapter import build_numactl_command, node_cpus


def make_app(nodes, memories=None, tt=50000):
    app = App()
    app.tt = tt
    cluster = Cluster()
    cluster.current_nodes = list(nodes)
    cluster.memories = list(memories or [1] * len(nodes))
    app.clusters.append(cluster)
    return app


class AsymSchedTests(unittest.TestCase):
    def test_reference_smoke(self):
        bandwidths = [[0, 3000000000, 1000000000, 1000000000], [3000000000, 0, 1000000000, 1000000000], [1000000000, 1000000000, 0, 7000000000], [1000000000, 1000000000, 7000000000, 0]]
        remote = [[0, 3000000000, 0, 0], [3000000000, 0, 0, 0], [0, 0, 0, 5000], [0, 0, 5000, 0]]
        apps = [make_app([0, 1], [200, 400]), make_app([2, 3], [30, 300])]
        _, placements, min_pid, moved = asymsched(apps, bandwidths, remote)
        self.assertEqual(min_pid, 16)
        self.assertTrue(moved)
        self.assertEqual(len(placements), 24)

    def test_node_cpus_parses_ranges_and_singletons(self):
        with tempfile.TemporaryDirectory() as tmp:
            cpulist = Path(tmp) / "node0" / "cpulist"
            cpulist.parent.mkdir()
            cpulist.write_text("0-1,4\n")
            self.assertEqual(node_cpus(0, tmp), [0, 1, 4])

    def test_kunpeng_numactl_wrapper(self):
        self.assertEqual(build_numactl_command([1, 0, 1], "./workload --size 4"), [
            "numactl", "--cpunodebind=0,1", "--membind=0,1", "./workload", "--size", "4"
        ])

    def test_kunpeng_numactl_wrapper_rejects_empty_command(self):
        with self.assertRaises(ValueError):
            build_numactl_command([0], [])

    def test_zero_remote_access_is_a_valid_no_migration_sample(self):
        apps = [make_app([0], [10])]
        _, placements, min_pid, moved = asymsched(apps, [[0]], [[0]])
        self.assertEqual(len(placements), 1)
        self.assertEqual(min_pid, 0)
        self.assertFalse(moved)
        self.assertEqual(apps[0].clusters[0].current_nodes, [0])


if __name__ == '__main__':
    unittest.main()
