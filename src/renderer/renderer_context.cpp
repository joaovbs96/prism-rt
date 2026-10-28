#include "renderer_context.h"

#include "../texture_service.h"

#include <embree4/rtcore.h>

#include <stdexcept>

namespace prism {

RendererContext::RendererContext(RendererOptions options)
    : m_options(std::move(options)), m_sssFallback(m_options.materials.sssFallback),
      m_logger(m_options.log, m_options.logLevel, m_options.debugCategories)
{
    Runtime::Instance().Configure(m_options);
}

RendererContext::~RendererContext()
{
    if (m_embree) rtcReleaseDevice(m_embree);
}

RTCDevice& RendererContext::embreeDevice() const
{
    std::lock_guard lock(m_mutex);
    if (!m_embree) {
        m_embree = rtcNewDevice(nullptr);
        if (!m_embree) throw std::runtime_error("Cannot create Embree device");
    }
    return m_embree;
}

std::shared_ptr<const color::ColorManagement> RendererContext::colorManagement() const
{
    std::lock_guard lock(m_mutex);
    if (!m_color) m_color = color::ColorManagement::Create(m_options.color);
    return m_color;
}

std::shared_ptr<const color::ColorManagement> RendererContext::setColor(color::Options const& options)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_color && m_color->requested() == options) return m_color;
    }
    auto created = color::ColorManagement::Create(options);
    std::lock_guard lock(m_mutex);
    m_color = std::move(created);
    return m_color;
}

bool RendererContext::sssFallback() const
{
    std::lock_guard lock(m_mutex);
    return m_sssFallback;
}

void RendererContext::setSssFallback(bool enabled)
{
    std::lock_guard lock(m_mutex);
    m_sssFallback = enabled;
}

std::shared_ptr<TextureService> RendererContext::textures() const
{
    std::lock_guard lock(m_mutex);
    if (!m_textures) m_textures = TextureService::Acquire(TextureServiceOptions::For(m_options));
    return m_textures;
}

DeviceResources RendererContext::deviceOptions() const
{
    DeviceResources devices(m_options.devices);
    devices.textures = textures();
    devices.progress = &m_progress;
    devices.logger = &m_logger;
    devices.varyingEmitters = m_varyingEmitters;
    return devices;
}

MaterialSettings RendererContext::materialSettings() const
{
    MaterialSettings settings;
    settings.color = colorManagement();
    settings.cacheDirectory = Runtime::Instance().settings().CacheRoot();
    settings.sssFallback = sssFallback();
    settings.generationCache = m_options.materials.generationCache;
    settings.mdlResourceMode = m_options.materials.mdlResourceMode;
    settings.progress = &m_progress;
    settings.logger = &m_logger;
    settings.varyingEmitters = m_varyingEmitters;
    return settings;
}

} // namespace prism
