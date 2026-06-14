#pragma once
#include <string>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <iostream>
#include "httplib.h"
#include <nlohmann/json.hpp>

class K8sClient {
public:
    K8sClient(bool in_cluster) {
        if (in_cluster) {
            base_url_ = "https://kubernetes.default.svc";
            std::ifstream token_file("/var/run/secrets/kubernetes.io/serviceaccount/token");
            if (token_file.is_open()) {
                std::stringstream buffer;
                buffer << token_file.rdbuf();
                token_ = buffer.str();
            }
        } else {
            // Simplified fallback for local testing without full kubeconfig parsing
            base_url_ = "http://localhost:8001"; // Assuming kubectl proxy
        }
    }

    bool create_namespace(const std::string& name, const std::string& submission_id) {
        nlohmann::json payload = {
            {"apiVersion", "v1"},
            {"kind", "Namespace"},
            {"metadata", {
                {"name", name},
                {"labels", {
                    {"app", "contestant"},
                    {"submission-id", submission_id}
                }}
            }}
        };
        return post("/api/v1/namespaces", payload);
    }

    bool create_resource_quota(const std::string& ns, const std::string& cpu_req, const std::string& mem_req, const std::string& cpu_lim, const std::string& mem_lim) {
        nlohmann::json payload = {
            {"apiVersion", "v1"},
            {"kind", "ResourceQuota"},
            {"metadata", {
                {"name", "contestant-quota"},
                {"namespace", ns}
            }},
            {"spec", {
                {"hard", {
                    {"requests.cpu", cpu_req},
                    {"requests.memory", mem_req},
                    {"limits.cpu", cpu_lim},
                    {"limits.memory", mem_lim},
                    {"pods", "5"}
                }}
            }}
        };
        return post("/api/v1/namespaces/" + ns + "/resourcequotas", payload);
    }

    bool create_network_policy(const std::string& ns) {
        nlohmann::json payload = {
            {"apiVersion", "networking.k8s.io/v1"},
            {"kind", "NetworkPolicy"},
            {"metadata", {
                {"name", "sandbox-isolation"},
                {"namespace", ns}
            }},
            {"spec", {
                {"podSelector", nlohmann::json::object()},
                {"policyTypes", {"Egress", "Ingress"}},
                {"egress", {{
                    {"ports", {
                        {{"protocol", "UDP"}, {"port", 53}},
                        {{"protocol", "TCP"}, {"port", 53}}
                    }},
                    {"to", nlohmann::json::array()}
                }}},
                {"ingress", {{
                    {"from", {{
                        {"namespaceSelector", {
                            {"matchLabels", {
                                {"kubernetes.io/metadata.name", "bot-fleet"}
                            }}
                        }}
                    }}}
                }}}
            }}
        };
        return post("/apis/networking.k8s.io/v1/namespaces/" + ns + "/networkpolicies", payload);
    }

    bool create_pod(const std::string& ns, const std::string& image, const std::string& runtime_class, const std::string& cpu_req, const std::string& mem_req, const std::string& cpu_lim, const std::string& mem_lim) {
        nlohmann::json payload = {
            {"apiVersion", "v1"},
            {"kind", "Pod"},
            {"metadata", {
                {"name", "exchange"},
                {"namespace", ns},
                {"labels", {
                    {"app", "exchange"}
                }}
            }},
            {"spec", {
                {"runtimeClassName", runtime_class},
                {"containers", {{
                    {"name", "exchange"},
                    {"image", image},
                    {"ports", {{
                        {"containerPort", 8080}
                    }}},
                    {"resources", {
                        {"requests", {
                            {"cpu", cpu_req},
                            {"memory", mem_req}
                        }},
                        {"limits", {
                            {"cpu", cpu_lim},
                            {"memory", mem_lim}
                        }}
                    }},
                    {"securityContext", {
                        {"allowPrivilegeEscalation", false},
                        {"readOnlyRootFilesystem", false}
                    }}
                }}}
            }}
        };
        return post("/api/v1/namespaces/" + ns + "/pods", payload);
    }

    bool create_service(const std::string& ns) {
        nlohmann::json payload = {
            {"apiVersion", "v1"},
            {"kind", "Service"},
            {"metadata", {
                {"name", "exchange-service"},
                {"namespace", ns}
            }},
            {"spec", {
                {"selector", {
                    {"app", "exchange"}
                }},
                {"ports", {{
                    {"port", 8080},
                    {"targetPort", 8080}
                }}}
            }}
        };
        return post("/api/v1/namespaces/" + ns + "/services", payload);
    }

private:
    std::string base_url_;
    std::string token_;

    bool post(const std::string& path, const nlohmann::json& payload) {
        httplib::Client cli(base_url_);
        if (!token_.empty()) {
            cli.set_bearer_token_auth(token_);
        }
        
        cli.set_connection_timeout(5, 0);  // 5 seconds timeout
        // Note: For production, configure proper TLS verification

        auto res = cli.Post(path, payload.dump(), "application/json");
        if (res && (res->status == 200 || res->status == 201 || res->status == 202)) {
            return true;
        }
        if (res && res->status == 409) {
            // Already exists, we can treat it as success or ignore
            return true; 
        }
        if (res) {
            std::cerr << "K8s API error: " << res->status << " " << res->body << std::endl;
        } else {
            std::cerr << "K8s API request failed: " << httplib::to_string(res.error()) << std::endl;
        }
        return false;
    }
};
