#pragma once

#include <cstdint>
#include <string>

namespace asymsched {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
};

/// Parse the legacy Demo request and return the legacy response JSON.
/// Throws json::JsonError or ValidationError on malformed input.
std::string handle_legacy_json(const std::string& body);

/// Parse a modern planner request and return a PlacementPlan JSON document.
std::string handle_plan_json(const std::string& body);

HttpResponse handle_http(const HttpRequest& request);

/// Blocking HTTP/1.1 server for the compatibility and planning APIs.
/// Returns 0 on clean shutdown; throws std::runtime_error on socket failure.
int run_server(const std::string& host, std::uint16_t port);

}  // namespace asymsched
