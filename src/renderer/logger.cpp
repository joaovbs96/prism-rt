#include "logger.h"

#include "renderer_options.h"

#include <algorithm>
#include <iostream>

namespace prism {

Logger::Logger(LogSink sink, LogLevel level, std::vector<std::string> debugCategories)
    : m_sink(std::move(sink)), m_level(level), m_debugCategories(std::move(debugCategories))
{
}

bool Logger::enabled(LogLevel level, std::string_view category) const
{
    if (level == LogLevel::Debug)
        return m_level == LogLevel::Debug ||
            std::find(m_debugCategories.begin(), m_debugCategories.end(), category) != m_debugCategories.end();
    return level >= m_level;
}

void Logger::log(LogLevel level, std::string_view category, std::string_view message) const
{
    if (!enabled(level, category)) return;
    if (m_sink) m_sink(level, category, message);
    else StandardSink(level, category, message);
}

bool Logger::once(std::string_view key) const
{
    std::lock_guard lock(m_mutex);
    if (m_seen.find(key) != m_seen.end()) return false;
    m_seen.emplace(key);
    return true;
}

void Logger::warnOnce(std::string_view category, std::string_view key, std::string_view message) const
{
    if (!enabled(LogLevel::Warning, category)) return;
    if (once(std::string(category) + "\n" + std::string(key))) warning(category, message);
}

void Logger::StandardSink(LogLevel level, std::string_view, std::string_view message)
{
    if (level == LogLevel::Debug) std::cout << message << std::flush;
    else std::cerr << "PrismRT: " << message << '\n';
}

namespace logging {

namespace {
thread_local Logger const* tCurrent = nullptr;
} // namespace

Logger const& Default()
{
    // Leaked: messages may come from worker threads at exit.
    static Logger* logger = [] {
        const auto options = RendererOptions::FromEnvironment();
        return new Logger({}, options.logLevel, options.debugCategories);
    }();
    return *logger;
}

Logger const& Current()
{
    return tCurrent ? *tCurrent : Default();
}

Bind::Bind(Logger const* logger) : m_previous(tCurrent) { tCurrent = logger; }
Bind::~Bind() { tCurrent = m_previous; }

} // namespace logging
} // namespace prism
