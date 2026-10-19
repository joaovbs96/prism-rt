#pragma once

// Loading timeline diagnostics (PRISM_LOAD_PROFILE=1). Each line carries the
// time since process start, a phase's wall duration and the process CPU time
// spent meanwhile; CPU / wall is the phase's average parallelism (phases
// running at the same time contribute to it). Disabled, a scope costs one
// cached flag test.

#include <string>
#include <string_view>

namespace prism::load_profile {

bool Enabled();
// Seconds since the process started (Windows), or since the first call.
double Now();
double ProcessCpuSeconds();
// "[load] t=<now> mark <name> <detail>"
void Mark(std::string_view name, std::string_view detail = {});

// "[load] t=<end> span <name> start=<s> wall=<w> cpu=<c> par=<c/w> <detail>"
class Scope {
public:
    explicit Scope(std::string_view name);
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void detail(std::string text) { if (m_enabled) m_detail = std::move(text); }
    ~Scope();

private:
    bool m_enabled;
    std::string m_name, m_detail;
    double m_start = 0.0, m_cpu = 0.0;
};

} // namespace prism::load_profile
