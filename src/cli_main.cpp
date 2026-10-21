#include "asymsched/api.h"
#include "asymsched/legacy.h"

#include <exception>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::string read_stdin() {
    return std::string(std::istreambuf_iterator<char>(std::cin),
                       std::istreambuf_iterator<char>());
}

int demo() {
    std::vector<asymsched::legacy::App> apps;
    asymsched::legacy::App first;
    first.tt = 50000;
    asymsched::legacy::Cluster first_cluster;
    first_cluster.memories = {200, 400};
    first_cluster.current_nodes = {0, 1};
    first.clusters.push_back(first_cluster);
    apps.push_back(first);

    asymsched::legacy::App second;
    second.tt = 50000;
    asymsched::legacy::Cluster second_cluster;
    second_cluster.memories = {30, 300};
    second_cluster.current_nodes = {2, 3};
    second.clusters.push_back(second_cluster);
    apps.push_back(second);

    const std::vector<std::vector<double>> bandwidths = {
        {0, 3000000000.0, 1000000000.0, 1000000000.0},
        {3000000000.0, 0, 1000000000.0, 1000000000.0},
        {1000000000.0, 1000000000.0, 0, 7000000000.0},
        {1000000000.0, 1000000000.0, 7000000000.0, 0}};
    const std::vector<std::vector<double>> remote = {
        {0, 3000000000.0, 0, 0},
        {3000000000.0, 0, 0, 0},
        {0, 0, 0, 5000.0},
        {0, 0, 0, 5000.0}};

    const auto result =
        asymsched::legacy::asymsched(apps, bandwidths, remote);
    std::cout << "min_pid=" << result.min_pid
              << " do_migration=" << (result.do_migration ? "true" : "false")
              << "\n";
    for (const auto& app : result.apps) {
        for (const auto& cluster : app.clusters) {
            std::cout << "origin=[";
            for (std::size_t index = 0; index < cluster.origin_nodes.size();
                 ++index) {
                if (index != 0) {
                    std::cout << ",";
                }
                std::cout << cluster.origin_nodes[index];
            }
            std::cout << "] current=[";
            for (std::size_t index = 0; index < cluster.current_nodes.size();
                 ++index) {
                if (index != 0) {
                    std::cout << ",";
                }
                std::cout << cluster.current_nodes[index];
            }
            std::cout << "]\n";
        }
    }
    return 0;
}

void usage(const char* program) {
    std::cout
        << "Usage: " << program << " <plan|legacy|demo>\n\n"
        << "  plan    read a modern planner request as JSON from stdin\n"
        << "  legacy  read a legacy Demo request as JSON from stdin\n"
        << "  demo    run the built-in four-node reference example\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        usage(argv[0]);
        return 2;
    }
    const std::string command = argv[1];
    if (command == "--help" || command == "-h" || command == "help") {
        usage(argv[0]);
        return 0;
    }
    try {
        if (command == "plan") {
            std::cout << asymsched::handle_plan_json(read_stdin()) << "\n";
            return 0;
        }
        if (command == "legacy") {
            std::cout << asymsched::handle_legacy_json(read_stdin()) << "\n";
            return 0;
        }
        if (command == "demo") {
            return demo();
        }
        std::cerr << "unknown command: " << command << "\n";
        usage(argv[0]);
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "asymsched-cli: " << error.what() << "\n";
        return 1;
    }
}
