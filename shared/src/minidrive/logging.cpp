#include "minidrive/logging.hpp"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <vector>

namespace minidrive::log {

namespace {

constexpr std::size_t ROTATING_FILE_MAX_SIZE = 5 * 1024 * 1024; // 5 MiB
constexpr std::size_t ROTATING_FILE_MAX_FILES = 3;

spdlog::level::level_enum to_spdlog(Level level) {
    switch(level) {
        case Level::trace: return spdlog::level::trace;
        case Level::debug: return spdlog::level::debug;
        case Level::info: return spdlog::level::info;
        case Level::warn: return spdlog::level::warn;
        case Level::err: return spdlog::level::err;
        case Level::critical: return spdlog::level::critical;
        case Level::off: return spdlog::level::off;
    }
    return spdlog::level::info;
}

} // namespace

Level level_from_string(const std::string& s, Level fallback) {
    if(s == "trace") return Level::trace;
    if(s == "debug") return Level::debug;
    if(s == "info") return Level::info;
    if(s == "warn" || s == "warning") return Level::warn;
    if(s == "error" || s == "err") return Level::err;
    if(s == "critical" || s == "crit") return Level::critical;
    if(s == "off") return Level::off;
    return fallback;
}

void init(const std::string& name, const std::string& file, Level level, bool also_console) {
    std::vector<spdlog::sink_ptr> sinks;
    if(!file.empty()) {
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            file, ROTATING_FILE_MAX_SIZE, ROTATING_FILE_MAX_FILES));
    }
    if(also_console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    std::shared_ptr<spdlog::logger> logger = sinks.empty()
        ? std::make_shared<spdlog::logger>(name)
        : std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
    logger->set_level(to_spdlog(level));
    // Flush every line, not just warn+: log volume here is one line per command/response, not
    // per-chunk (chunk transfers use a separate binary framing that never reaches this logger),
    // so the throughput cost is negligible - and an unflushed line lost to a crash would defeat
    // the point of an audit trail.
    logger->flush_on(spdlog::level::trace);
    spdlog::set_default_logger(logger);
}

} // namespace minidrive::log