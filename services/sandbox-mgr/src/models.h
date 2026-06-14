#pragma once
#include <string>
#include <chrono>
#include <nlohmann/json.hpp>

struct DeploymentStatus {
    std::string submission_id;
    std::string status; // building, ready, failed, torn_down
    std::string endpoint_url;
    std::string namespace_name;
    std::string image_tag;
    std::string error_msg;
    std::string build_logs;
    std::string created_at;
    std::string updated_at;
};

inline void to_json(nlohmann::json& j, const DeploymentStatus& ds) {
    j = nlohmann::json{
        {"submission_id", ds.submission_id},
        {"status", ds.status},
        {"created_at", ds.created_at},
        {"updated_at", ds.updated_at}
    };
    if (!ds.endpoint_url.empty()) j["endpoint_url"] = ds.endpoint_url;
    if (!ds.namespace_name.empty()) j["namespace"] = ds.namespace_name;
    if (!ds.image_tag.empty()) j["image_tag"] = ds.image_tag;
    if (!ds.error_msg.empty()) j["error_msg"] = ds.error_msg;
    if (!ds.build_logs.empty()) j["build_logs"] = ds.build_logs;
}

struct BuildAndDeployRequest {
    std::string submission_id;
    std::string language; // "go", "cpp", "rust"
    std::string source_url;
    std::string team_id;
};

inline void from_json(const nlohmann::json& j, BuildAndDeployRequest& req) {
    if (j.contains("submission_id")) j.at("submission_id").get_to(req.submission_id);
    if (j.contains("language")) j.at("language").get_to(req.language);
    if (j.contains("source_url")) j.at("source_url").get_to(req.source_url);
    if (j.contains("team_id")) j.at("team_id").get_to(req.team_id);
}

inline std::string current_time_str() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now_c));
    return std::string(buf);
}
