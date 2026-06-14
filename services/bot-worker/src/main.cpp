#include <iostream>
#include <memory>
#include <csignal>
#include <atomic>

#include "config.h"
#include "reporter.h"
#include "worker.h"

std::atomic<bool> keep_running(true);

void signal_handler(int) {
    keep_running = false;
}

std::string generate_worker_id() {
    // simple 8 char random hex
    const char* v = "0123456789abcdef";
    std::string res(8, '0');
    for (int i = 0; i < 8; ++i) {
        res[i] = v[rand() % 16];
    }
    return res;
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    srand(time(nullptr));

    Config cfg = load_config();
    if (cfg.worker_id.empty()) {
        cfg.worker_id = generate_worker_id();
    }

    std::cout << "starting bot worker"
              << " worker_id=" << cfg.worker_id
              << " max_bots=" << cfg.max_bots_per_worker << std::endl;

    auto reporter = std::make_shared<Reporter>(cfg);
    reporter->run();

    auto worker = std::make_unique<Worker>(cfg, reporter);
    worker->run();

    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "shutting down bot worker..." << std::endl;
    worker->stop();
    worker->wait();
    reporter->stop();
    std::cout << "bot worker stopped" << std::endl;

    return 0;
}
