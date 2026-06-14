#pragma once

#include <string>
#include <vector>
#include <cstdlib>
#include <sstream>

struct Config {
    std::string worker_id;
    std::vector<std::string> redpanda_brokers;
    std::string command_topic;
    std::string telemetry_topic;
    int max_bots_per_worker;
    std::string consumer_group;
};

inline std::string get_env(const std::string& key, const std::string& fallback) {
    const char* val = std::getenv(key.c_str());
    if (val && *val != '\0') {
        return std::string(val);
    }
    return fallback;
}

inline int get_env_int(const std::string& key, int fallback) {
    const char* val = std::getenv(key.c_str());
    if (val && *val != '\0') {
        try {
            return std::stoi(val);
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

inline std::vector<std::string> split_brokers(const std::string& s) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, ',')) {
        tokens.push_back(token);
    }
    return tokens;
}

inline Config load_config() {
    Config cfg;
    cfg.worker_id = get_env("WORKER_ID", "");
    cfg.redpanda_brokers = split_brokers(get_env("REDPANDA_BROKERS", "localhost:19092"));
    cfg.command_topic = get_env("COMMAND_TOPIC", "bot-commands");
    cfg.telemetry_topic = get_env("TELEMETRY_TOPIC", "telemetry-raw");
    cfg.max_bots_per_worker = get_env_int("MAX_BOTS_PER_WORKER", 1000);
    cfg.consumer_group = get_env("CONSUMER_GROUP", "bot-workers");
    return cfg;
}
