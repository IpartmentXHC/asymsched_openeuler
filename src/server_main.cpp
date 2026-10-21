#include "asymsched/api.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

namespace {

void usage(const char* program) {
    std::cout << "Usage: " << program
              << " [--host ADDRESS] [--port PORT]\n\n"
                 "Serve the legacy /api/asymsched_once endpoint and the\n"
                 "modern /api/plan endpoint.  The default is 127.0.0.1:5000.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 5000;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            usage(argv[0]);
            return 0;
        }
        if (argument == "--host" && index + 1 < argc) {
            host = argv[++index];
            continue;
        }
        if (argument == "--port" && index + 1 < argc) {
            try {
                const unsigned long parsed = std::stoul(argv[++index]);
                if (parsed > 65535) {
                    throw std::out_of_range("port");
                }
                port = static_cast<std::uint16_t>(parsed);
            } catch (const std::exception&) {
                std::cerr << "invalid port\n";
                return 2;
            }
            continue;
        }
        std::cerr << "unknown argument: " << argument << "\n";
        usage(argv[0]);
        return 2;
    }

    try {
        return asymsched::run_server(host, port);
    } catch (const std::exception& error) {
        std::cerr << "asymsched-server: " << error.what() << "\n";
        return 1;
    }
}
