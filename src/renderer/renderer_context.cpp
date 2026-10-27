#include "renderer_context.h"

#include "../texture_service.h"

namespace prism {

RendererContext::RendererContext(RendererOptions options)
    : m_options(std::move(options)), m_sssFallback(m_options.materials.sssFallback)
{
    Runtime::Instance().Configure(m_options);
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

DeviceOptions RendererContext::deviceOptions() const
{
    DeviceOptions devices = m_options.devices;
    devices.textures = textures();
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
    return settings;
}

} // namespace prism
