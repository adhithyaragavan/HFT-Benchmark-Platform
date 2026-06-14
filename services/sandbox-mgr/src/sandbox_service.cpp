#include "sandbox_service.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <array>
#include <regex>

SandboxService::SandboxService(const Config& cfg) : cfg_(cfg) {
    if (cfg_.mode == "k8s") {
        try {
            k8s_client_ = std::make_unique<K8sClient>(cfg_.in_cluster);
            std::cout << "kubernetes client initialized successfully" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "failed to create k8s client, falling back to local mode: " << e.what() << std::endl;
            cfg_.mode = "local";
        }
    }

    // Start background worker threads
    size_t pool_size = 2; // Fixed thread pool size to limit CPU/Memory thrashing
    for (size_t i = 0; i < pool_size; ++i) {
        workers_.emplace_back([this]() {
            while (true) {
                BuildAndDeployRequest req;
                {
                    std::unique_lock<std::mutex> lock(queue_mu_);
                    queue_cv_.wait(lock, [this]() { return stop_workers_ || !queue_.empty(); });
                    if (stop_workers_ && queue_.empty()) {
                        return;
                    }
                    req = std::move(queue_.front());
                    queue_.pop();
                }
                try {
                    build_and_deploy(req);
                } catch (const std::exception& e) {
                    std::cerr << "Error in worker thread during build_and_deploy: " << e.what() << std::endl;
                }
            }
        });
    }
}

SandboxService::~SandboxService() {
    {
        std::unique_lock<std::mutex> lock(queue_mu_);
        stop_workers_ = true;
    }
    queue_cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void SandboxService::enqueue_build(const BuildAndDeployRequest& req) {
    auto now = current_time_str();
    auto status = std::make_shared<DeploymentStatus>();
    status->submission_id = req.submission_id;
    status->status = "pending_build";
    status->created_at = now;
    status->updated_at = now;

    {
        std::lock_guard<std::mutex> lock(mu_);
        deployments_[req.submission_id] = status;
    }

    {
        std::unique_lock<std::mutex> lock(queue_mu_);
        queue_.push(req);
    }
    queue_cv_.notify_one();
}

std::string SandboxService::dockerfile_template(const std::string& language) {
    if (language == "go") {
        return "# Build stage\n"
               "FROM golang:1.22-alpine AS builder\n"
               "WORKDIR /app\n"
               "COPY . .\n"
               "RUN go mod download\n"
               "RUN CGO_ENABLED=0 GOOS=linux go build -o /exchange .\n\n"
               "# Runtime stage\n"
               "FROM gcr.io/distroless/static-debian12:nonroot\n"
               "COPY --from=builder /exchange /exchange\n"
               "EXPOSE 8080\n"
               "USER nonroot:nonroot\n"
               "ENTRYPOINT [\"/exchange\"]\n";
    } else if (language == "cpp") {
        return "# Build stage\n"
               "FROM debian:bookworm AS builder\n"
               "RUN apt-get update && apt-get install -y g++ cmake make\n"
               "WORKDIR /app\n"
               "COPY . .\n"
               "RUN if [ -f CMakeLists.txt ]; then \\\n"
               "      apt-get update && apt-get install -y cmake && \\\n"
               "      mkdir build && cd build && cmake .. && make -j2; \\\n"
               "    elif [ -f Makefile ]; then \\\n"
               "      make -j2; \\\n"
               "    else \\\n"
               "      g++ -O2 -std=c++20 -I. -o exchange main.cpp; \\\n"
               "    fi\n"
               "RUN find . -name 'exchange' -type f | head -1 | xargs -I{} cp {} /exchange || cp build/exchange /exchange 2>/dev/null || true\n\n"
               "# Runtime stage\n"
               "FROM debian:bookworm-slim\n"
               "RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates && rm -rf /var/lib/apt/lists/*\n"
               "RUN useradd -m -s /bin/bash contestant\n"
               "COPY --from=builder /exchange /exchange\n"
               "RUN chmod +x /exchange\n"
               "EXPOSE 8080\n"
               "USER contestant\n"
               "ENTRYPOINT [\"/exchange\"]\n";
    } else if (language == "rust") {
        return "# Build stage\n"
               "FROM rust:1.78 AS builder\n"
               "WORKDIR /app\n"
               "COPY . .\n"
               "RUN CARGO_BUILD_JOBS=2 cargo build --release\n"
               "RUN cp target/release/$(ls target/release/ | grep -v '\\.d$' | head -1) /exchange 2>/dev/null || \\\n"
               "    find target/release -maxdepth 1 -type f -executable | head -1 | xargs -I{} cp {} /exchange\n\n"
               "# Runtime stage\n"
               "FROM debian:bookworm-slim\n"
               "RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates && rm -rf /var/lib/apt/lists/*\n"
               "RUN useradd -m -s /bin/bash contestant\n"
               "COPY --from=builder /exchange /exchange\n"
               "RUN chmod +x /exchange\n"
               "EXPOSE 8080\n"
               "USER contestant\n"
               "ENTRYPOINT [\"/exchange\"]\n";
    }
    return "";
}

std::string exec_cmd(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    int status = pclose(pipe);
    if (status != 0) {
        throw std::runtime_error("Command failed with status " + std::to_string(status) + ": " + result);
    }
    return result;
}

void SandboxService::fail_deployment(std::shared_ptr<DeploymentStatus> status, const std::string& err_msg) {
    std::lock_guard<std::mutex> lock(mu_);
    status->status = "failed";
    status->error_msg = err_msg;
    status->updated_at = current_time_str();
    std::cerr << "Deployment failed: " << err_msg << std::endl;
}

void SandboxService::build_and_deploy(const BuildAndDeployRequest& req) {
    std::shared_ptr<DeploymentStatus> status;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = deployments_.find(req.submission_id);
        if (it != deployments_.end()) {
            status = it->second;
            status->status = "building";
            status->updated_at = current_time_str();
        } else {
            auto now = current_time_str();
            status = std::make_shared<DeploymentStatus>();
            status->submission_id = req.submission_id;
            status->status = "building";
            status->created_at = now;
            status->updated_at = now;
            deployments_[req.submission_id] = status;
        }
    }

    if (!std::regex_match(req.submission_id, std::regex("^[a-zA-Z0-9_-]+$"))) {
        fail_deployment(status, "invalid submission_id format");
        return;
    }

    if (cfg_.mode == "local") {
        build_and_deploy_local(req, status);
    } else if (cfg_.mode == "k8s") {
        build_and_deploy_k8s(req, status);
    } else {
        fail_deployment(status, "unsupported mode: " + cfg_.mode);
    }
}

void SandboxService::build_and_deploy_local(const BuildAndDeployRequest& req, std::shared_ptr<DeploymentStatus> status) {
    std::string short_id = req.submission_id.substr(0, 8);
    std::string image_tag = "contestant-" + short_id + ":latest";
    std::string container_name = "contestant-" + short_id;

    std::string dockerfile = dockerfile_template(req.language);
    if (dockerfile.empty()) {
        fail_deployment(status, "unsupported language: " + req.language);
        return;
    }

    std::cout << "building contestant image (local mode) submission_id=" << req.submission_id << " image_tag=" << image_tag << std::endl;

    std::string source_path = "/tmp/submissions/" + req.submission_id;
    std::string init_dir_cmd = "mkdir -p " + source_path;
    try {
        exec_cmd(init_dir_cmd.c_str());
    } catch (...) {}

    // Download and extract archive if source_url is provided
    if (!req.source_url.empty()) {
        std::string download_cmd = "curl -sfL -o " + source_path + "/source.archive \"" + req.source_url + "\"";
        try {
            exec_cmd(download_cmd.c_str());
            
            // Extract using tar or unzip
            std::string extract_cmd = "tar -C " + source_path + " -xzf " + source_path + "/source.archive >/dev/null 2>&1 || "
                                      "tar -C " + source_path + " -xf " + source_path + "/source.archive >/dev/null 2>&1 || "
                                      "unzip -o -d " + source_path + " " + source_path + "/source.archive >/dev/null 2>&1 || true";
            exec_cmd(extract_cmd.c_str());
        } catch (const std::exception& e) {
            std::cerr << "Warning: Failed to download/extract source code from " << req.source_url << ": " << e.what() << std::endl;
        }
    }

    // Write the Dockerfile
    std::string write_file_cmd = "cat > " + source_path + "/Dockerfile << 'DOCKERFILE_EOF'\n" + dockerfile + "DOCKERFILE_EOF";
    try {
        exec_cmd(write_file_cmd.c_str());
    } catch (const std::exception& e) {
        fail_deployment(status, std::string("failed to write Dockerfile: ") + e.what());
        return;
    }

    std::string build_cmd = "docker build -t " + image_tag + " --memory=2g --cpu-quota=200000 " + source_path + " 2>&1";
    try {
        status->build_logs = exec_cmd(build_cmd.c_str());
    } catch (const std::exception& e) {
        fail_deployment(status, std::string("docker build failed: ") + e.what());
        return;
    }

    std::cout << "image built successfully image_tag=" << image_tag << std::endl;

    // Ensure the 'platform' network exists
    try {
        exec_cmd("docker network create platform >/dev/null 2>&1 || true");
    } catch (...) {}

    std::string rm_cmd = "docker rm -f " + container_name + " >/dev/null 2>&1";
    try { exec_cmd(rm_cmd.c_str()); } catch (...) {}

    // Run container with tmpfs for writeable /tmp and limit parallel builder thrashing
    std::string run_cmd = "docker run -d --name " + container_name + 
                          " --memory=2g --cpus=2 --read-only --tmpfs /tmp:rw,noexec,nosuid,size=64m --security-opt=no-new-privileges:true --cap-drop=ALL "
                          "-p 0:8080 --network=platform " + image_tag + " 2>&1";
    try {
        std::string run_output = exec_cmd(run_cmd.c_str());
        std::cout << "container started container_id=" << run_output.substr(0, 12) << std::endl;
        
        // Let's get the port mapping dynamically
        std::string port_cmd = "docker port " + container_name + " 8080";
        std::string port_out = exec_cmd(port_cmd.c_str());
        
        std::string mapped_port = "8080"; // fallback
        size_t colon_pos = port_out.find_last_of(':');
        if (colon_pos != std::string::npos) {
            std::string port_str = port_out.substr(colon_pos + 1);
            port_str.erase(std::remove(port_str.begin(), port_str.end(), '\n'), port_str.end());
            port_str.erase(std::remove(port_str.begin(), port_str.end(), '\r'), port_str.end());
            port_str.erase(std::remove(port_str.begin(), port_str.end(), ' '), port_str.end());
            if (!port_str.empty() && std::all_of(port_str.begin(), port_str.end(), ::isdigit)) {
                mapped_port = port_str;
            }
        }
        
        std::string endpoint_url = "http://localhost:" + mapped_port;
        
        {
            std::lock_guard<std::mutex> lock(mu_);
            status->status = "ready";
            status->endpoint_url = endpoint_url;
            status->updated_at = current_time_str();
        }
    } catch (const std::exception& e) {
        fail_deployment(status, std::string("docker run failed: ") + e.what());
    }
}

void SandboxService::build_and_deploy_k8s(const BuildAndDeployRequest& req, std::shared_ptr<DeploymentStatus> status) {
    std::string short_id = req.submission_id.substr(0, 8);
    std::string namespace_name = "contestant-" + short_id;
    
    std::cout << "starting k8s deployment submission_id=" << req.submission_id << " namespace=" << namespace_name << std::endl;

    if (!k8s_client_->create_namespace(namespace_name, req.submission_id)) {
        fail_deployment(status, "failed to create namespace");
        return;
    }

    if (!k8s_client_->create_resource_quota(namespace_name, cfg_.cpu_request, cfg_.memory_request, cfg_.cpu_limit, cfg_.memory_limit)) {
        fail_deployment(status, "failed to create resource quota");
        return;
    }

    if (!k8s_client_->create_network_policy(namespace_name)) {
        fail_deployment(status, "failed to create network policy");
        return;
    }

    std::string image_name = cfg_.registry_url + "/contestant-" + short_id + ":latest";
    if (!k8s_client_->create_pod(namespace_name, image_name, cfg_.runtime_class, cfg_.cpu_request, cfg_.memory_request, cfg_.cpu_limit, cfg_.memory_limit)) {
        fail_deployment(status, "failed to create pod");
        return;
    }

    if (!k8s_client_->create_service(namespace_name)) {
        fail_deployment(status, "failed to create service");
        return;
    }

    std::string endpoint_url = "http://exchange-service." + namespace_name + ".svc.cluster.local:8080";

    {
        std::lock_guard<std::mutex> lock(mu_);
        status->status = "ready";
        status->endpoint_url = endpoint_url;
        status->namespace_name = namespace_name;
        status->updated_at = current_time_str();
    }
    std::cout << "k8s deployment ready submission_id=" << req.submission_id << " endpoint=" << endpoint_url << std::endl;
}

bool SandboxService::teardown(const std::string& submission_id) {
    std::shared_ptr<DeploymentStatus> status;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = deployments_.find(submission_id);
        if (it == deployments_.end()) return false;
        status = it->second;
    }

    if (cfg_.mode == "local") {
        std::string short_id = submission_id.substr(0, 8);
        std::string rm_cmd = "docker rm -f contestant-" + short_id;
        try { exec_cmd(rm_cmd.c_str()); } catch (...) {}
    } else {
        std::cout << "k8s teardown - would delete namespace " << submission_id << std::endl;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        status->status = "torn_down";
        status->updated_at = current_time_str();
    }
    return true;
}

std::shared_ptr<DeploymentStatus> SandboxService::get_status(const std::string& submission_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = deployments_.find(submission_id);
    if (it != deployments_.end()) {
        return it->second;
    }
    return nullptr;
}

std::string SandboxService::get_build_logs(const std::string& submission_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = deployments_.find(submission_id);
    if (it != deployments_.end()) {
        return it->second->build_logs;
    }
    return "";
}
