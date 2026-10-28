#pragma once

// A renderer's note that some material emits with varying emission
// (Material::variesEmission), made as soon as the material is classified, long
// before a device prepares the scene. The renderer's devices subscribe to start
// work that only such scenes need (the Vulkan device compiles the raygen
// shader variant with PRISM_VARYING_EMITTERS while the other materials still
// load). The note outlives the scene: a later scene of the renderer without
// such emitters at worst costs a variant that goes unused. Each renderer has
// its own (RendererContext::varyingEmitters); materials note on the one in
// their MaterialSettings.

#include "load_profile.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace prism {

class VaryingEmitterSignal {
public:
    // Notes made so far.
    uint64_t Notes() const { return m_notes.load(); }

    // Called for each material that emits with varying emission; listeners
    // run on the calling thread.
    void Note()
    {
        if (m_notes.fetch_add(1) == 0) prism::load_profile::Mark("varying_emitter.first");
        std::vector<std::function<void()>> call;
        {
            std::lock_guard lock(m_mutex);
            for (auto const& [token, listener] : m_listeners) call.push_back(listener);
        }
        for (auto const& listener : call) listener();
    }

    // Subscribes; returns a token for Unsubscribe. A listener should be cheap
    // and idempotent.
    int Subscribe(std::function<void()> listener)
    {
        std::lock_guard lock(m_mutex);
        const int token = m_nextToken++;
        m_listeners.emplace(token, std::move(listener));
        return token;
    }

    void Unsubscribe(int token)
    {
        std::lock_guard lock(m_mutex);
        m_listeners.erase(token);
    }

private:
    std::atomic<uint64_t> m_notes{0};
    std::mutex m_mutex;
    std::map<int, std::function<void()>> m_listeners;
    int m_nextToken = 1;
};

} // namespace prism
