#pragma once

#include <string>

namespace minidrive::log {

// Mirrors spdlog::level::level_enum, kept as our own type so the CLI-facing surface
// (--log-level strings) doesn't leak spdlog's enum naming into server/client code.
enum class Level { trace, debug, info, warn, err, critical, off };

// Parses a --log-level argument ("trace"/"debug"/"info"/"warn"/"error"/"critical"/"off",
// case-sensitive). Unrecognized input returns `fallback` rather than failing, since a bad log
// level shouldn't prevent the server/client from starting.
Level level_from_string(const std::string& s, Level fallback = Level::info);

// Initializes the process-wide default spdlog logger.
//   name          - logger name, prefixed on every line (e.g. "server", "client").
//   file          - rotating log file path (5 MiB x 3 backups). Empty disables the file sink.
//   level         - minimum severity that reaches any sink.
//   also_console  - additionally mirror to stdout. Server-only. The client must always pass
//                   false: its stdout is the tested OK/ERROR protocol contract (see
//                   docs/protocol.md), and a stray log line there would corrupt it.
// If both `file` is empty and `also_console` is false, logging is a no-op (no sinks attached).
// Not thread-safe to call concurrently with logging calls; intended to run once at startup
// before any worker threads are spawned.
void init(const std::string& name, const std::string& file, Level level, bool also_console);

} // namespace minidrive::log