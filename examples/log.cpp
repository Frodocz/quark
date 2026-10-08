#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "quark/QuarkConfig.h"
#include "quark/QuarkLog.h"

int main(int argc, char** argv) {
    const char* config_path = argc > 1 ? argv[1] : QUARK_EXAMPLE_LOG_CONFIG;

    // 1. Parse the application's config once. A parse error is reported by quark on stderr, since
    //    logging is not started yet.
    auto cfg = quark::QuarkConfig::load_file(config_path);
    if (!cfg) {
        std::cerr << "failed to load config file: " << config_path << std::endl;
        return 1;
    }

    // 2. Start logging for the lifetime of main() from the config's [log] tables: every logger is
    //    created here.
    quark::ScopedLog scoped_log{*cfg};

    // 3. Look the loggers up once and keep the handles, they are cheap to copy.
    quark::Logger app = quark::get_logger("app");
    quark::Logger net = quark::get_logger("net");
    quark::Logger monitor = quark::get_logger("monitor");

    QLOG_INFO(app, "hello from logger '{}', version {}.{}.{}", app.name(), QUARK_VERSION_MAJOR,
              QUARK_VERSION_MINOR, QUARK_VERSION_PATCH);
    // The same config object also carries the application's own settings.
    QLOG_INFO(app, "server listening on {}:{}", cfg->get_or("server.host", std::string{"0.0.0.0"}),
              cfg->get_or("server.port", int64_t{80}));
    QLOG_DEBUG(app, "filtered out: [log.app] level = \"Info\"");
    QLOG_WARN(app, "values are formatted in the backend: {} {:.3f} {}", 42, 3.14159, std::string{"str"});
    QLOG_DEBUG(monitor, "monitor logs everything: cpu={}%", 12.5);

    // 4. Handles are safe to share across threads.
    std::vector<std::thread> workers;
    for (int id = 0; id < 3; ++id) {
        workers.emplace_back([net, id] {
            QLOG_INFO(net, "filtered out until net is raised to Info: worker {}", id);
            QLOG_WARN(net, "worker {} lost connection, retrying", id);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    // 5. Only the level can change at runtime.
    net.set_level(quark::LogLevel::Info);
    QLOG_INFO(net, "now visible after set_level(Info)");

    // 6. quark's own records go to the "quark" logger, so they never mix with the application's:
    //   ... QuarkConfig.cpp:66  WARNING  quark  [quark.config] failed to parse config string at 1:7: ...
    (void)quark::QuarkConfig::load_string("key = = 1");
    (void)quark::get_logger("typo");  // undeclared logger: quark warns and returns an invalid handle

    QLOG_ERROR(app, "an error, see logs/app.log, logs/net.log and logs/quark.log");

    // ~ScopedLog flushes everything and stops the backend thread.
    return 0;
}
