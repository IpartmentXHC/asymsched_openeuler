import math
import itertools
import sys

try:
    from .models import Cluster, App, Placement
except ImportError:  # Support the original direct-script invocation.
    from models import Cluster, App, Placement

def do_placements(num_nodes, apps):
    """ 通过排列组合枚举的方式，产生所有可能的放置策略。

    参数：
        num_nodes：最多可以放置的节点数量。
        apps：App类的列表。

    返回值：
        Placement类的列表，涵盖了cluster所有可能的放置策略。比如：

        [
            {0, 0, [[0, 1, 2]], [[3, 4]], [[5]]},
            ...
        ]
    """

    # 计算一共需要使用的节点数量
    num_used_nodes = 0
    for app in apps:
        for cluster in app.clusters:
            num_used_nodes += len(cluster.current_nodes)

    permutations = itertools.permutations(range(0, num_nodes), num_used_nodes)

    placements = []
    for p_id, permutation in enumerate(permutations):
        next_node = 0
        placements.append(Placement(len(apps)))
        for app_id, app in enumerate(apps):
            for cluster_id, cluster in enumerate(app.clusters):
                placements[p_id].placements[app_id].append([])
                for _ in cluster.current_nodes:
                    placements[p_id].placements[app_id][cluster_id].append(permutation[next_node])
                    next_node += 1

    return placements

def calculate_cbw(cluster_placement, bandwidths):
    """ 计算每个cluster的最大远程访问带宽之和

    参数：
        cluster_placement：一位整型数组，表示当前cluster的放置策略
        bandwidths: 机器参数，长宽相同的二维整型列表，bandwidths[i][j]指的是
            节点i到节点j的最大带宽。

    返回值:
        一个整型数值，表示cluster中所有节点直接最大远程访问之和
    """

    sum_cbw = 0
    for node_from in cluster_placement:
        for node_to in cluster_placement:
            if node_from != node_to:
                sum_cbw += bandwidths[node_from][node_to]
    return sum_cbw


def calculate_pwbw(placements, apps, bandwidths):
    """ 计算每个placement的权重，并返回最大的权重值

    参数：
        placements: Placement类的列表
        apps: App类的列表
        bandwidths: 机器参数，长宽相同的二维整型列表，bandwidths[i][j]指的是
            节点i到节点j的最大带宽。

    返回值：
        一个整型数值，表示所有placements中最大的权重值
    """

    max_wbw = -float('inf')
    for placement in placements:
        for app_id, app_placement in enumerate(placement.placements):
            for cluster_id, cluster_placement in enumerate(app_placement):
                sum_cbw = calculate_cbw(cluster_placement, bandwidths)
                placement.wbw += sum_cbw * apps[app_id].clusters[cluster_id].weight

        max_wbw = max(max_wbw, placement.wbw)

    return max_wbw

def calculate_amm(app_id, app_placement, placement, apps):
    """ 计算每个app中所有cluster需要迁移内存的大小
    """

    for cluster_id, cluster_placement in enumerate(app_placement):
        for node_id, node in enumerate(cluster_placement):
            if apps[app_id].clusters[cluster_id].current_nodes[node_id] != node:
                placement.mm += apps[app_id].clusters[cluster_id].memories[node_id]


def calculate_pmm(wbw_filter, placements, apps):
    """ 计算每个placement需要迁移的内存大小，
        并返回需要最小内存迁移大小相应的placement的标号

    """

    min_pmm = sys.maxsize
    min_pid = -1
    for placement_id, placement in enumerate(placements):
        if placement.wbw >= wbw_filter:
            for app_id, app_placement in enumerate(placement.placements):
                calculate_amm(app_id, app_placement, placement, apps)

            if placement.mm < min_pmm:
                min_pmm = placement.mm
                min_pid = placement_id

    assert min_pid > -1
    assert min_pmm < sys.maxsize

    return min_pid

def check_placement_diff(apps, placement):
    """ 判断新的放置策略和旧的放置策略是否有区别
    """
    for app_id, app_placement in enumerate(placement.placements):
        for cluster_id, cluster_placement in enumerate(app_placement):
            for node_id, node in enumerate(cluster_placement):
                if apps[app_id].clusters[cluster_id].current_nodes[node_id] != node:
                    return True

    return False


def check_migration(apps, placement):
    """ 计算是否需要迁移
    """
    if not check_placement_diff(apps, placement):
        return False

    for app_id, app in enumerate(apps):
        migrate_filter = apps[app_id].tt * 0.05
        migrate_factor = apps[app_id].tm
        for cluster_id, cluster in enumerate(app.clusters):
            for node_id, _ in enumerate(cluster.current_nodes):
                if node_id != placement.placements[app_id][cluster_id][node_id]:
                    migrate_factor += app.clusters[cluster_id].memories[node_id] * 0.3

        if migrate_factor > migrate_filter:
            return False

    return True

def migrate(apps, placement):
    """ 节点迁移
    """

    # 计算是否需要执行迁移
    do_migration = check_migration(apps, placement)

    # 执行迁移
    if do_migration:
        for app_id, app_placements in enumerate(placement.placements):
            for cluster_id, cluster_placement in enumerate(app_placements):
                apps[app_id].clusters[cluster_id].origin_nodes = apps[app_id].clusters[cluster_id].current_nodes
                apps[app_id].clusters[cluster_id].current_nodes = cluster_placement

    return do_migration


def asymsched(apps, bandwidths, remote_access):
    """ 产生cluster的放置和调度策略。

    参数：
        apps: App类的列表。
        bandwidths: 机器参数，长宽相同的二维整型列表，bandwidths[i][j]指的是
            节点i到节点j的最大带宽。
        remote_access: 监测数据，长宽相同的二维整型列表，remote[i][j]指的是
            节点i到节点j的当前的使用带宽。

    返回值：
        apps：App的列表
        placements：所有可能的放置策略的列表
        min_pid：最优的方策策略的标号
        do_migration：布尔值，是否执行了migration
    """

    # 计算每个app所有的cluster的权重
    for app in apps:
        for cluster in app.clusters:
            cluster.rbw = 0
            for node_from in cluster.current_nodes:
                for node_to in cluster.current_nodes:
                    if node_from != node_to:
                        cluster.rbw += remote_access[node_from][node_to]

            # Zero observed remote traffic means no affinity pressure. Keep the
            # cluster eligible so an all-zero sample still selects least migration.
            cluster.weight = math.log(cluster.rbw) if cluster.rbw > 0 else 0.0

    # 计算所有可能的放置策略
    num_nodes = len(bandwidths)
    placements = do_placements(num_nodes, apps)

    # 计算每个placement的权重
    max_wbw = calculate_pwbw(placements, apps, bandwidths)

    # 计算每个placements需要迁移的内存大小
    min_pid = calculate_pmm(0.9 * max_wbw, placements, apps)

    # 线程迁移
    do_migration = migrate(apps, placements[min_pid])

    return apps, placements, min_pid, do_migration

def asymsched_test():
    """ 正确性测试

    模拟CPU一共有四个节点。
    运行两个app，每个app有一个cluster，每个cluster使用两个node。
    """

    test_bandwidths = [
        [0, 3000000000, 1000000000, 1000000000],
        [3000000000, 0, 1000000000, 1000000000],
        [1000000000, 1000000000, 0, 7000000000],
        [1000000000, 1000000000, 7000000000, 0]
    ]

    test_remote_access = [
        [0, 3000000000, 0, 0],
        [3000000000, 0, 0, 0],
        [0, 0, 0, 5000],
        [0, 0, 5000, 0]
    ]

    test_apps = []
    test_apps.append(App())
    test_apps[0].tt = 50000
    test_apps[0].clusters.append(Cluster())
    test_apps[0].clusters[0].memories = [200, 400]
    test_apps[0].clusters[0].current_nodes = [0, 1]

    test_apps.append(App())
    test_apps[1].tt = 50000
    test_apps[1].clusters.append(Cluster())
    test_apps[1].clusters[0].memories = [30, 300]
    test_apps[1].clusters[0].current_nodes = [2, 3]

    _, _, test_min_pid, test_do_migration = asymsched(test_apps, test_bandwidths, test_remote_access)

    print(test_min_pid, test_do_migration)
    for app in test_apps:
        for cluster in app.clusters:
            print(cluster.origin_nodes, "==>", cluster.current_nodes)

if __name__ == '__main__':
    asymsched_test()
