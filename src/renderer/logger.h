#pragma once

// A renderer's diagnostics (docs/sdk_design.md, 2.5): warnings and debug
// output go to its sink (the CLI's standard error, the Hydra delegate's
// TF_WARN, later Python's logging), with debug output per category ("mdl",
// "gpu", "lights") and warnings that should appear once deduplicated per
// renderer, not per process.
//
// Code that has its renderer's logger at hand uses it; deep code (MDL
// compilation, device internals) uses logging::Current(), the logger bound
// to the calling thread (logging::Bind, which renderers install on the
// threads they run) or else the process default, which writes to standard
// error with the environment's debug categories (PRISM_MDL_VERBOSE and so on).

#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// LogLevel and LogSink are public (the renderer's options).
#include "../../include/prism/options.h"

namespace prism {

class Logger {
public:
    // An empty sink writes to standard error (debug output to standard
    // output, as the verbose switches always did).
    explicit Logger(LogSink sink = {}, LogLevel level = LogLevel::Warning,
                    std::vector<std::string> debugCategories = {});

    // Messages below `level` are dropped, except debug messages of the
    // debug categories.
    bool enabled(LogLevel level, std::string_view category) const;
    void log(LogLevel level, std::string_view category, std::string_view message) const;
    void debug(std::string_view category, std::string_view message) const { log(LogLevel::Debug, category, message); }
    void warning(std::string_view category, std::string_view message) const
    {
        log(LogLevel::Warning, category, message);
    }
    // True the first time this logger sees `key`.
    bool once(std::string_view key) const;
    // A warning this logger reports once per `key`.
    void warnOnce(std::string_view category, std::string_view key, std::string_view message) const;

    // Standard error (debug: standard output) with a "PrismRT: " prefix.
    static void StandardSink(LogLevel level, std::string_view category, std::string_view message);

private:
    LogSink m_sink;
    LogLevel m_level;
    std::vector<std::string> m_debugCategories;
    mutable std::mutex m_mutex;
    mutable std::set<std::string, std::less<>> m_seen;
};

namespace logging {

// The logger bound to the calling thread, else the process default.
Logger const& Current();
// The process default: standard error, the environment's debug categories.
Logger const& Default();

// Binds a logger to the calling thread for a scope.
class Bind {
public:
    explicit Bind(Logger const* logger);
    ~Bind();
    Bind(Bind const&) = delete;
    Bind& operator=(Bind const&) = delete;

private:
    Logger const* m_previous;
};

// Debug output of `category` is enabled for the calling thread's logger.
inline bool Debug(std::string_view category) { return Current().enabled(LogLevel::Debug, category); }

} // namespace logging
} // namespace prism
