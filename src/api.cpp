#include "asymsched/api.h"

#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "asymsched/json.h"
#include "asymsched/legacy.h"
#include "asymsched/planning.h"

namespace asymsched {
namespace {

const json::Value& object_field(const json::Value& object, const char* name) {
    return object.at(name);
}

std::vector<std::vector<double>> number_matrix(const json::Value& value,
                                               const char* name) {
    if (!value.is_array()) {
        throw ValidationError(std::string(name) + " must be an array");
    }
    std::vector<std::vector<double>> matrix;
    matrix.reserve(value.array().size());
    for (const auto& row_value : value.array()) {
        if (!row_value.is_array()) {
            throw ValidationError(std::string(name) +
                                  " must be an array of arrays");
        }
        std::vector<double> row;
        row.reserve(row_value.array().size());
        for (const auto& item : row_value.array()) {
            if (!item.is_number()) {
                throw ValidationError(std::string(name) +
                                      " must contain numbers");
            }
            row.push_back(item.number());
        }
        matrix.push_back(std::move(row));
    }
    return matrix;
}

std::vector<std::vector<std::uint64_t>> integer_matrix(
    const json::Value& value, const char* name) {
    if (!value.is_array()) {
        throw ValidationError(std::string(name) + " must be an array");
    }
    std::vector<std::vector<std::uint64_t>> matrix;
    matrix.reserve(value.array().size());
    for (const auto& row_value : value.array()) {
        if (!row_value.is_array()) {
            throw ValidationError(std::string(name) +
                                  " must be an array of arrays");
        }
        std::vector<std::uint64_t> row;
        row.reserve(row_value.array().size());
        for (const auto& item : row_value.array()) {
            if (!item.is_integer_literal()) {
                throw ValidationError(std::string(name) +
                                      " must contain integer counts");
            }
            row.push_back(item.as_u64());
        }
        matrix.push_back(std::move(row));
    }
    return matrix;
}

std::vector<NodeId> node_vector(const json::Value& value, const char* name) {
    if (!value.is_array()) {
        throw ValidationError(std::string(name) + " must be an array");
    }
    std::vector<NodeId> nodes;
    nodes.reserve(value.array().size());
    for (const auto& item : value.array()) {
        if (!item.is_integer_literal()) {
            throw ValidationError(std::string(name) +
                                  " must contain nonnegative integers");
        }
        const std::uint64_t parsed = item.as_u64();
        if (parsed > std::numeric_limits<NodeId>::max()) {
            throw ValidationError(std::string(name) +
                                  " contains an out-of-range node id");
        }
        nodes.push_back(static_cast<NodeId>(parsed));
    }
    return nodes;
}

std::vector<double> number_vector(const json::Value& value, const char* name) {
    if (!value.is_array()) {
        throw ValidationError(std::string(name) + " must be an array");
    }
    std::vector<double> values;
    values.reserve(value.array().size());
    for (const auto& item : value.array()) {
        if (!item.is_number()) {
            throw ValidationError(std::string(name) +
                                  " must contain numbers");
        }
        values.push_back(item.number());
    }
    return values;
}

std::size_t max_placements(const json::Value& root) {
    if (!root.contains("max_placements")) {
        return 100000;
    }
    const auto& value = object_field(root, "max_placements");
    if (!value.is_integer_literal()) {
        throw ValidationError("max_placements must be a positive integer");
    }
    const std::uint64_t parsed = value.as_u64();
    if (parsed > std::numeric_limits<std::size_t>::max()) {
        throw ValidationError("max_placements is out of range");
    }
    return static_cast<std::size_t>(parsed);
}

legacy::Result parse_legacy(const std::string& body) {
    const json::Value root = json::parse(body);
    if (!root.is_object()) {
        throw ValidationError("request body must be a JSON object");
    }
    const auto& apps_value = object_field(root, "apps");
    if (!apps_value.is_array()) {
        throw ValidationError("apps must be an array");
    }
    std::vector<legacy::App> apps;
    apps.reserve(apps_value.array().size());
    for (const auto& app_value : apps_value.array()) {
        if (!app_value.is_object()) {
            throw ValidationError("each app must be an object");
        }
        legacy::App app;
        app.tm = object_field(app_value, "tm").number();
        app.tt = object_field(app_value, "tt").number();
        const auto& clusters_value = object_field(app_value, "clusters");
        if (!clusters_value.is_array()) {
            throw ValidationError("clusters must be an array");
        }
        app.clusters.reserve(clusters_value.array().size());
        for (const auto& cluster_value : clusters_value.array()) {
            if (!cluster_value.is_object()) {
                throw ValidationError("each cluster must be an object");
            }
            legacy::Cluster cluster;
            cluster.memories =
                number_vector(object_field(cluster_value, "memories"),
                              "memories");
            cluster.current_nodes =
                node_vector(object_field(cluster_value, "current_nodes"),
                            "current_nodes");
            app.clusters.push_back(std::move(cluster));
        }
        apps.push_back(std::move(app));
    }
    return legacy::asymsched(
        std::move(apps), number_matrix(object_field(root, "bandwidths"),
                                       "bandwidths"),
        number_matrix(object_field(root, "remote_access"), "remote_access"),
        max_placements(root));
}

PlacementPlan parse_plan(const std::string& body) {
    const json::Value root = json::parse(body);
    if (!root.is_object()) {
        throw ValidationError("request body must be a JSON object");
    }
    const auto& apps_value = object_field(root, "apps");
    if (!apps_value.is_array()) {
        throw ValidationError("apps must be an array");
    }
    std::vector<AppSnapshot> apps;
    apps.reserve(apps_value.array().size());
    for (const auto& app_value : apps_value.array()) {
        if (!app_value.is_object()) {
            throw ValidationError("each app must be an object");
        }
        AppSnapshot app;
        app.app_id = object_field(app_value, "app_id").string();
        app.current_nodes =
            node_vector(object_field(app_value, "current_nodes"),
                        "current_nodes");
        app.memory_gb =
            number_vector(object_field(app_value, "memory_gb"), "memory_gb");
        app.accesses =
            integer_matrix(object_field(app_value, "accesses"), "accesses");
        app.tm_seconds = object_field(app_value, "tm_seconds").number();
        app.tt_seconds = object_field(app_value, "tt_seconds").number();
        apps.push_back(std::move(app));
    }
    return plan_placement(
        number_matrix(object_field(root, "bandwidths"), "bandwidths"), apps,
        max_placements(root));
}

json::Value nodes_json(const std::vector<NodeId>& nodes) {
    json::Value::Array result;
    result.reserve(nodes.size());
    for (NodeId node : nodes) {
        result.push_back(json::Value::make_integer(node));
    }
    return json::Value::make_array(std::move(result));
}

json::Value legacy_nodes_json(const std::vector<legacy::Node>& nodes) {
    json::Value::Array result;
    result.reserve(nodes.size());
    for (legacy::Node node : nodes) {
        result.push_back(json::Value::make_integer(node));
    }
    return json::Value::make_array(std::move(result));
}

json::Value numbers_json(const std::vector<double>& values) {
    json::Value::Array result;
    result.reserve(values.size());
    for (double value : values) {
        result.push_back(json::Value::make_number(value));
    }
    return json::Value::make_array(std::move(result));
}

json::Value legacy_to_json(const legacy::Result& result) {
    json::Value::Array apps;
    apps.reserve(result.apps.size());
    for (const auto& app : result.apps) {
        json::Value::Array clusters;
        clusters.reserve(app.clusters.size());
        for (const auto& cluster : app.clusters) {
            json::Value::Object item;
            item["rbw"] = json::Value::make_number(cluster.rbw);
            item["weight"] = json::Value::make_number(cluster.weight);
            item["memories"] = numbers_json(cluster.memories);
            item["origin_nodes"] = legacy_nodes_json(cluster.origin_nodes);
            item["current_nodes"] = legacy_nodes_json(cluster.current_nodes);
            clusters.push_back(json::Value::make_object(std::move(item)));
        }
        json::Value::Object item;
        item["tm"] = json::Value::make_number(app.tm);
        item["tt"] = json::Value::make_number(app.tt);
        item["clusters"] = json::Value::make_array(std::move(clusters));
        apps.push_back(json::Value::make_object(std::move(item)));
    }

    json::Value::Array placements;
    placements.reserve(result.placements.size());
    for (const auto& placement : result.placements) {
        json::Value::Array app_placements;
        app_placements.reserve(placement.placements.size());
        for (const auto& cluster_placements : placement.placements) {
            json::Value::Array clusters;
            clusters.reserve(cluster_placements.size());
            for (const auto& nodes : cluster_placements) {
                clusters.push_back(legacy_nodes_json(nodes));
            }
            app_placements.push_back(
                json::Value::make_array(std::move(clusters)));
        }
        json::Value::Object item;
        item["wbw"] = json::Value::make_number(placement.wbw);
        item["mm"] = json::Value::make_number(placement.mm);
        item["placements"] =
            json::Value::make_array(std::move(app_placements));
        placements.push_back(json::Value::make_object(std::move(item)));
    }

    json::Value::Object root;
    root["apps"] = json::Value::make_array(std::move(apps));
    root["placements"] = json::Value::make_array(std::move(placements));
    root["min_pid"] = json::Value::make_integer(result.min_pid);
    root["do_migration"] = json::Value::make_boolean(result.do_migration);
    return json::Value::make_object(std::move(root));
}

json::Value plan_to_json(const PlacementPlan& plan) {
    json::Value::Array candidate;
    candidate.reserve(plan.candidate.size());
    for (const auto& nodes : plan.candidate) {
        candidate.push_back(nodes_json(nodes));
    }
    json::Value::Array final_nodes;
    final_nodes.reserve(plan.final.size());
    for (const auto& nodes : plan.final) {
        final_nodes.push_back(nodes_json(nodes));
    }

    json::Value::Array clusters;
    clusters.reserve(plan.clusters.size());
    for (const auto& cluster : plan.clusters) {
        json::Value::Object item;
        item["app_id"] = json::Value::make_string(cluster.app_id);
        item["nodes"] = nodes_json(cluster.nodes);
        item["remote_bandwidth_bps"] =
            json::Value::make_integer(cluster.remote_bandwidth_bps);
        item["weight"] = json::Value::make_number(cluster.weight);
        clusters.push_back(json::Value::make_object(std::move(item)));
    }

    json::Value::Array decisions;
    decisions.reserve(plan.decisions.size());
    for (const auto& decision : plan.decisions) {
        json::Value::Object item;
        item["app_id"] = json::Value::make_string(decision.app_id);
        item["current_nodes"] = nodes_json(decision.current_nodes);
        item["candidate_nodes"] = nodes_json(decision.candidate_nodes);
        item["final_nodes"] = nodes_json(decision.final_nodes);
        item["migration_gb"] =
            json::Value::make_number(decision.migration_gb);
        item["estimated_seconds"] =
            json::Value::make_number(decision.estimated_seconds);
        item["approved"] = json::Value::make_boolean(decision.approved);
        item["reason"] = json::Value::make_string(decision.reason);
        decisions.push_back(json::Value::make_object(std::move(item)));
    }

    json::Value::Object root;
    root["candidate"] = json::Value::make_array(std::move(candidate));
    root["final"] = json::Value::make_array(std::move(final_nodes));
    root["clusters"] = json::Value::make_array(std::move(clusters));
    root["decisions"] = json::Value::make_array(std::move(decisions));
    root["best_score"] = json::Value::make_number(plan.best_score);
    root["score"] = json::Value::make_number(plan.score);
    root["migration_gb"] = json::Value::make_number(plan.migration_gb);
    root["qualifying_count"] =
        json::Value::make_integer(plan.qualifying_count);
    root["status"] = json::Value::make_string(plan.status);
    return json::Value::make_object(std::move(root));
}

std::string error_body(const std::string& message) {
    json::Value::Object root;
    root["error"] = json::Value::make_string(message);
    return json::dump(json::Value::make_object(std::move(root)));
}

std::string path_only(const std::string& path) {
    const std::size_t query = path.find('?');
    return query == std::string::npos ? path : path.substr(0, query);
}

}  // namespace

std::string handle_legacy_json(const std::string& body) {
    return json::dump(legacy_to_json(parse_legacy(body)));
}

std::string handle_plan_json(const std::string& body) {
    return json::dump(plan_to_json(parse_plan(body)));
}

HttpResponse handle_http(const HttpRequest& request) {
    const std::string path = path_only(request.path);
    try {
        if (path == "/health" && request.method == "GET") {
            return HttpResponse{200, "application/json; charset=utf-8",
                                "{\"status\":\"ok\"}"};
        }
        if (path == "/api/asymsched_once") {
            if (request.method != "POST") {
                return HttpResponse{405, "application/json; charset=utf-8",
                                    error_body("method not allowed")};
            }
            return HttpResponse{200, "application/json; charset=utf-8",
                                handle_legacy_json(request.body)};
        }
        if (path == "/api/plan") {
            if (request.method != "POST") {
                return HttpResponse{405, "application/json; charset=utf-8",
                                    error_body("method not allowed")};
            }
            return HttpResponse{200, "application/json; charset=utf-8",
                                handle_plan_json(request.body)};
        }
        return HttpResponse{404, "application/json; charset=utf-8",
                            error_body("not found")};
    } catch (const std::invalid_argument& error) {
        return HttpResponse{400, "application/json; charset=utf-8",
                            error_body(error.what())};
    } catch (const json::JsonError& error) {
        return HttpResponse{400, "application/json; charset=utf-8",
                            error_body(error.what())};
    } catch (const std::exception& error) {
        return HttpResponse{500, "application/json; charset=utf-8",
                            error_body(error.what())};
    }
}

}  // namespace asymsched
