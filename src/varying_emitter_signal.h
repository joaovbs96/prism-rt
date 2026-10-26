#pragma once

// A process-wide note that some material emits with varying emission
// (Material::variesEmission), made as soon as the material is classified, long
// before a device prepares the scene. Devices subscribe to start work that
// only such scenes need (the Vulkan device compiles the raygen shader variant
// with PRISM_VARYING_EMITTERS while the other materials still load).
// The note outlives the scene: a later scene without such emitters at worst
// costs a variant that goes unused.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace prism::varying_emitters {

namespace detail {
struct State {
    std::atomic<uint64_t> notes{0};
    std::mutex mutex;
    std::map<int, std::function<void()>> listeners;
    int nextToken = 1;
};
inline State& state()
{
    static State* s = new State; // never destroyed: notes may come from worker threads at exit
    return *s;
}
} // namespace detail

// Notes made so far.
inline uint64_t Notes() { return detail::state().notes.load(); }

// Called for each material that emits with varying emission; listeners run on the calling thread.
inline void Note()
{
    auto& s = detail::state();
    ++s.notes;
    std::vector<std::function<void()>> call;
    {
        std::lock_guard lock(s.mutex);
        for (auto const& [token, listener] : s.listeners) call.push_back(listener);
    }
    for (auto const& listener : call) listener();
}

// Subscribes; returns a token for Unsubscribe. A listener should be cheap and idempotent.
inline int Subscribe(std::function<void()> listener)
{
    auto& s = detail::state();
    std::lock_guard lock(s.mutex);
    const int token = s.nextToken++;
    s.listeners.emplace(token, std::move(listener));
    return token;
}

inline void Unsubscribe(int token)
{
    auto& s = detail::state();
    std::lock_guard lock(s.mutex);
    s.listeners.erase(token);
}

} // namespace prism::varying_emitters
