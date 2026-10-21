#include "asymsched/api.h"
#include "asymsched/json.h"
#include "test_framework.h"

#include <cmath>
#include <string>

namespace {

const char* kLegacyRequest = R"({
  "apps": [
    {"tm": 0.0, "tt": 50000, "clusters": [
      {"memories": [200, 400], "current_nodes": [0, 1]}]},
    {"tm": 0.0, "tt": 50000, "clusters": [
      {"memories": [30, 300], "current_nodes": [2, 3]}]}
  ],
  "bandwidths": [
    [0, 3000000000, 1000000000, 1000000000],
    [3000000000, 0, 1000000000, 1000000000],
    [1000000000, 1000000000, 0, 7000000000],
    [1000000000, 1000000000, 7000000000, 0]
  ],
  "remote_access": [
    [0, 3000000000, 0, 0],
    [3000000000, 0, 0, 0],
    [0, 0, 0, 5000],
    [0, 0, 5000, 0]
  ]
})";

const char* kPlanRequest = R"({
  "bandwidths": [[0, 1, 1], [1, 0, 20], [1, 2, 0]],
  "apps": [{
    "app_id": "a",
    "current_nodes": [0, 1],
    "memory_gb": [2, 1],
    "accesses": [[0, 0, 4], [0, 0, 3], [0, 0, 0]],
    "tm_seconds": 0.0,
    "tt_seconds": 100.0
  }]
})";

}  // namespace

TEST(api_legacy_endpoint_matches_reference_demo) {
    const auto value = asymsched::json::parse(
        asymsched::handle_legacy_json(kLegacyRequest));
    CHECK(value.is_object());
    CHECK_EQ(value.at("min_pid").as_u64(), 16);
    CHECK(value.at("do_migration").boolean());
    CHECK_EQ(value.at("apps").array().size(), 2);
    CHECK_EQ(value.at("placements").array().size(), 24);
    CHECK_EQ(value.at("apps")
                 .array()[0]
                 .at("clusters")
                 .array()[0]
                 .at("current_nodes")
                 .array()[0]
                 .as_u64(),
             2);
}

TEST(api_plan_endpoint_returns_typed_plan) {
    const auto value =
        asymsched::json::parse(asymsched::handle_plan_json(kPlanRequest));
    CHECK_EQ(value.at("status").string(), "planned");
    CHECK_EQ(value.at("candidate").array()[0].array()[0].as_u64(), 2);
    CHECK_EQ(value.at("candidate").array()[0].array()[1].as_u64(), 1);
    CHECK_NEAR(value.at("best_score").number(), 22.0 * std::log(7.0), 1e-9);
    CHECK_EQ(value.at("migration_gb").number(), 2);
    CHECK_EQ(value.at("decisions").array()[0].at("reason").string(),
             "approved");
}

TEST(api_rejects_negative_access_counts) {
    const std::string request = R"({
      "bandwidths": [[0]],
      "apps": [{
        "app_id": "a", "current_nodes": [0], "memory_gb": [1],
        "accesses": [[-1]], "tm_seconds": 0, "tt_seconds": 1
      }]
    })";
    CHECK_THROWS(asymsched::handle_plan_json(request));
    const auto response = asymsched::handle_http(
        asymsched::HttpRequest{"POST", "/api/plan", request});
    CHECK_EQ(response.status, 400);
}

TEST(api_http_routes_and_errors) {
    const auto health = asymsched::handle_http(
        asymsched::HttpRequest{"GET", "/health", ""});
    CHECK_EQ(health.status, 200);
    CHECK_EQ(health.body, "{\"status\":\"ok\"}");

    const auto missing = asymsched::handle_http(
        asymsched::HttpRequest{"GET", "/missing", ""});
    CHECK_EQ(missing.status, 404);

    const auto wrong_method = asymsched::handle_http(
        asymsched::HttpRequest{"GET", "/api/asymsched_once", ""});
    CHECK_EQ(wrong_method.status, 405);

    const auto malformed = asymsched::handle_http(
        asymsched::HttpRequest{"POST", "/api/asymsched_once", "{"});
    CHECK_EQ(malformed.status, 400);

    const auto legacy = asymsched::handle_http(
        asymsched::HttpRequest{"POST", "/api/asymsched_once", kLegacyRequest});
    CHECK_EQ(legacy.status, 200);
}
