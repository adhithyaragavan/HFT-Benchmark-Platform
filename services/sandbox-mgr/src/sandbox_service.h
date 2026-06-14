#pragma once
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <vector>
#include "config.h"
#include "models.h"
#include "k8s_client.h"

class SandboxService {
public:
    explicit SandboxService(const Config& cfg);
    ~SandboxService();

    void enqueue_build(const BuildAndDeployRequest& req);
    void build_and_deploy(const BuildAndDeployRequest& req);
    bool teardown(const std::string& submission_id);
    std::shared_ptr<DeploymentStatus> get_status(const std::string& submission_id);
    std::string get_build_logs(const std::string& submission_id);

private:
    Config cfg_;
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<DeploymentStatus>> deployments_;
    std::unique_ptr<K8sClient> k8s_client_;

    // Thread pool for build tasks
    std::vector<std::thread> workers_;
    std::queue<BuildAndDeployRequest> queue_;
    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    bool stop_workers_ = false;

    void build_and_deploy_local(const BuildAndDeployRequest& req, std::shared_ptr<DeploymentStatus> status);
    void build_and_deploy_k8s(const BuildAndDeployRequest& req, std::shared_ptr<DeploymentStatus> status);
    
    void fail_deployment(std::shared_ptr<DeploymentStatus> status, const std::string& err_msg);
    std::string dockerfile_template(const std::string& language);
};
