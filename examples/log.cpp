#include <atomic>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "quark/QuarkConfig.h"
#include "quark/QuarkLog.h"

// What a library does: look its logger up once, never touch the backend.
class Connector {
public:
    void connect(int id) { QLOG_WARN(log_, "worker {} lost connection, retrying", id); }

private:
    quark::Logger* log_{quark::get_logger("net")};
};

// usage: quark_example_log [config.toml] [log_hft | log_service]
int main(int argc, char** argv) {
    const char* config_path = argc > 1 ? argv[1] : QUARK_EXAMPLE_LOG_CONFIG;
    const char* profile = argc > 2 ? argv[2] : "log_hft";

    auto cfg = quark::QuarkConfig::load_file(config_path);
    if (!cfg) {
        std::cerr << "failed to load config file: " << config_path << std::endl;
        return 1;
    }

    // The process owns logging: init_log() once, from [<profile>] and [<profile>.<name>].
    const quark::LogConfig log_config = quark::LogConfig::load_from_config(*cfg, profile);
    quark::ScopedLog scoped_log{log_config};

    // With manual_backend the process drives the backend from a thread of its choosing.
    std::atomic<bool> running{true};
    std::optional<std::jthread> poll_thread;
    if (log_config.backend.manual_backend) {
        poll_thread.emplace([&running] {
            quark::LogPoller poller;
            while (running.load(std::memory_order_relaxed)) {
                poller.poll_for(std::chrono::microseconds{50});
            }
        });
    }

    quark::Logger* app = quark::get_logger("app");
    QLOG_INFO(app, "logging with profile [{}], version {}.{}.{}", profile, QUARK_VERSION_MAJOR,
              QUARK_VERSION_MINOR, QUARK_VERSION_PATCH);
    QLOG_INFO(app, "server listening on {}:{}", cfg->get_or("server.host", std::string{"0.0.0.0"}),
              cfg->get_or("server.port", int64_t{80}));
    QLOG_DEBUG(app, "filtered out: app's level is Info in both profiles");

    std::vector<std::thread> workers;
    for (int id = 0; id < 3; ++id) {
        workers.emplace_back([id] {
            quark::Frontend::preallocate();
            Connector{}.connect(id);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    // Not in the config: created with the default options.
    QLOG_INFO(quark::get_logger("strategy.mm"), "undeclared loggers get the default options");

    // quark's own records go to the "quark" logger.
    (void)quark::QuarkConfig::load_string("key = = 1");

    running = false;
    return 0;
}
