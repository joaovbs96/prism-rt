#include "load_profile.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace prism::load_profile {
namespace {

#if defined(_WIN32)
uint64_t Ticks(FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
#endif

void Write(const std::string& line)
{
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);
}

} // namespace

bool Enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("PRISM_LOAD_PROFILE");
        return value && *value && std::string_view(value) != "0";
    }();
    return enabled;
}

double Now()
{
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point origin = [] {
        Clock::time_point now = Clock::now();
#if defined(_WIN32)
        FILETIME created, exited, kernel, user, current;
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            GetSystemTimeAsFileTime(&current);
            now -= std::chrono::microseconds((Ticks(current) - Ticks(created)) / 10);
        }
#endif
        return now;
    }();
    return std::chrono::duration<double>(Clock::now() - origin).count();
}

double ProcessCpuSeconds()
{
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    return double(Ticks(kernel) + Ticks(user)) * 1e-7;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0.0;
    return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
        double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1e-6;
#endif
}

void Mark(std::string_view name, std::string_view detail)
{
    if (!Enabled()) return;
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "[load] t=%.3f mark ", Now());
    Write(std::string(prefix) + std::string(name) +
          (detail.empty() ? std::string() : " " + std::string(detail)));
}

Scope::Scope(std::string_view name) : m_enabled(Enabled())
{
    if (!m_enabled) return;
    m_name = name;
    m_start = Now();
    m_cpu = ProcessCpuSeconds();
}

Scope::~Scope()
{
    if (!m_enabled) return;
    const double end = Now();
    const double wall = end - m_start;
    const double cpu = ProcessCpuSeconds() - m_cpu;
    char text[256];
    std::snprintf(text, sizeof(text), "[load] t=%.3f span %s start=%.3f wall=%.3f cpu=%.3f par=%.1f",
                  end, m_name.c_str(), m_start, wall, cpu, wall > 1e-6 ? cpu / wall : 0.0);
    Write(std::string(text) + (m_detail.empty() ? std::string() : " " + m_detail));
}

} // namespace prism::load_profile
