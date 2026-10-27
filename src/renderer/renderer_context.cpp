#include "renderer_context.h"

namespace prism {

RendererContext::RendererContext(RendererOptions options) : m_options(std::move(options)) {}

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

MaterialSettings RendererContext::materialSettings() const
{
    MaterialSettings settings;
    settings.color = colorManagement();
    settings.cacheDirectory = m_options.CacheRoot();
    return settings;
}

} // namespace prism
