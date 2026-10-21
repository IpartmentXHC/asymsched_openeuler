"""Linux/Kunpeng execution helpers for an AsymSched placement.

This module replaces the AMD-specific control plane with standard Linux
process affinity and NUMA policy. It does not emulate AMD IBS sampling.
"""

import os
from pathlib import Path
import shlex


_SYS_NODE = Path("/sys/devices/system/node")


def node_cpus(node, sys_node=_SYS_NODE):
    """Return online CPU ids belonging to a NUMA node."""
    text = (Path(sys_node) / f"node{node}" / "cpulist").read_text().strip()
    cpus = []
    for part in text.split(","):
        bounds = [int(value) for value in part.split("-")]
        cpus.extend(range(bounds[0], bounds[-1] + 1) if len(bounds) == 2 else bounds)
    return cpus


def cpus_for_nodes(nodes, sys_node=_SYS_NODE):
    """Return a sorted, de-duplicated CPU list for NUMA nodes."""
    return sorted({cpu for node in nodes for cpu in node_cpus(node, sys_node)})


def apply_process_affinity(nodes, sys_node=_SYS_NODE):
    """Bind the current process to CPUs attached to *nodes*."""
    cpus = cpus_for_nodes(nodes, sys_node)
    if not cpus:
        raise ValueError("at least one NUMA node with online CPUs is required")
    os.sched_setaffinity(0, cpus)
    return cpus


def build_numactl_command(nodes, command):
    """Build a reproducible process-level NUMA wrapper command."""
    if not nodes:
        raise ValueError("at least one NUMA node is required")
    if isinstance(command, str):
        command = shlex.split(command)
    if not command:
        raise ValueError("a workload command is required")
    nodes = sorted(set(nodes))
    return ["numactl", "--cpunodebind=" + ",".join(map(str, nodes)),
            "--membind=" + ",".join(map(str, nodes)), *command]
