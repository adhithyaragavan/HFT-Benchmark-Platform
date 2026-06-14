#include <drogon/drogon.h>
#include <iostream>
#include <memory>
#include "config.h"
#include "sandbox_service.h"
#include "handler.h"

std::shared_ptr<SandboxService> g_sandbox_svc;

int main() {
    std::cout << "Starting sandbox manager..." << std::endl;

    Config cfg = load_config();
    std::cout << "Port: " << cfg.port << " Mode: " << cfg.mode << std::endl;

    g_sandbox_svc = std::make_shared<SandboxService>(cfg);

    register_sandbox_handlers();

    std::string host = "0.0.0.0";
    int port = std::stoi(cfg.port);

    std::cout << "Sandbox manager listening on " << host << ":" << port << std::endl;

    drogon::app()
        .addListener(host, port)
        .setClientMaxBodySize(20 * 1024 * 1024)
        .setThreadNum(4)
        .run();

    return 0;
}
