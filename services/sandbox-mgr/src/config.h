#pragma once
#include <string>
#include <cstdlib>

struct Config {
    std::string port;
    std::string mode;
    std::string database_url;
    std::string kube_context;
    bool in_cluster;
    std::string registry_url;
    std::string runtime_class;
    std::string builder_image;
    std::string cpu_request;
    std::string cpu_limit;
    std::string memory_request;
    std::string memory_limit;
};

inline std::string get_env(const char* key, const std::string& fallback) {
    const char* val = std::getenv(key);
    return val ? std::string(val) : fallback;
}

inline Config load_config() {
    Config c;
    c.port = get_env("SANDBOX_PORT", "8095");
    c.mode = get_env("SANDBOX_MODE", "local");
    c.database_url = get_env("DATABASE_URL", "postgres://platform:platform@localhost:5432/benchmarks");
    c.kube_context = get_env("KUBE_CONTEXT", "");
    c.in_cluster = get_env("IN_CLUSTER", "false") == "true";
    c.registry_url = get_env("REGISTRY_URL", "localhost:5001");
    c.runtime_class = get_env("RUNTIME_CLASS", "gvisor");
    c.builder_image = get_env("BUILDER_IMAGE", "gcr.io/kaniko-project/executor:latest");
    c.cpu_request = get_env("CPU_REQUEST", "500m");
    c.cpu_limit = get_env("CPU_LIMIT", "4");
    c.memory_request = get_env("MEMORY_REQUEST", "512Mi");
    c.memory_limit = get_env("MEMORY_LIMIT", "8Gi");
    return c;
}
