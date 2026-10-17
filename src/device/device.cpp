#include "device.h"
#include "cpu_device.h"
#ifdef PRISM_HAS_METAL
#include "metal/metal_device.h"
#endif

#include <cctype>

namespace prism::device {

const char* ToString(DeviceKind kind)
{
    switch (kind) {
    case DeviceKind::Cpu: return "cpu";
    case DeviceKind::CpuReference: return "cpu-reference";
    case DeviceKind::Vulkan: return "vulkan";
    case DeviceKind::Metal: return "metal";
    }
    return "unknown";
}

const char* ToString(DevicePreference preference)
{
    switch (preference) {
    case DevicePreference::Auto: return "auto";
    case DevicePreference::Cpu: return "cpu";
    case DevicePreference::CpuReference: return "cpu-reference";
    case DevicePreference::Vulkan: return "vulkan";
    case DevicePreference::Metal: return "metal";
    }
    return "unknown";
}

bool ParseDevicePreference(std::string_view text, DevicePreference* preference)
{
    std::string lower(text);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "auto") *preference = DevicePreference::Auto;
    else if (lower == "cpu") *preference = DevicePreference::Cpu;
    else if (lower == "cpu-reference") *preference = DevicePreference::CpuReference;
    else if (lower == "vulkan") *preference = DevicePreference::Vulkan;
    else if (lower == "metal") *preference = DevicePreference::Metal;
    else return false;
    return true;
}

namespace {

// Returns null (with a reason) if the device is unavailable.
std::unique_ptr<RenderDevice> TryCreate(DeviceKind kind, std::string* reason)
{
    switch (kind) {
    case DeviceKind::Cpu:
        return std::make_unique<CpuDevice>();
    case DeviceKind::CpuReference:
        return std::make_unique<CpuDevice>(true);
    case DeviceKind::Vulkan:
        if (reason) *reason = "the Vulkan device is not part of this build";
        return nullptr;
    case DeviceKind::Metal:
#ifdef PRISM_HAS_METAL
        return MetalDevice::Create(reason);
#else
        if (reason) *reason = "the Metal device is not part of this build";
        return nullptr;
#endif
    }
    return nullptr;
}

// GPU kinds to try for Auto, in order of preference on this platform.
std::vector<DeviceKind> AutoOrder()
{
#ifdef __APPLE__
    return {DeviceKind::Metal, DeviceKind::Vulkan};
#else
    return {DeviceKind::Vulkan};
#endif
}

} // namespace

std::vector<DeviceInfo> AvailableDevices()
{
    std::vector<DeviceInfo> devices;
    for (DeviceKind kind : {DeviceKind::Metal, DeviceKind::Vulkan}) {
        if (auto device = TryCreate(kind, nullptr)) devices.push_back(device->Info());
    }
    devices.push_back(CpuDevice(true).Info());
    devices.push_back(CpuDevice().Info());
    return devices;
}

std::unique_ptr<RenderDevice> CreateDevice(DevicePreference preference, std::string* note)
{
    std::vector<DeviceKind> order;
    switch (preference) {
    case DevicePreference::Auto: order = AutoOrder(); break;
    case DevicePreference::Cpu: break;
    case DevicePreference::CpuReference:
        if (note) note->clear();
        return std::make_unique<CpuDevice>(true);
    case DevicePreference::Vulkan: order = {DeviceKind::Vulkan}; break;
    case DevicePreference::Metal: order = {DeviceKind::Metal}; break;
    }
    std::string reasons;
    for (DeviceKind kind : order) {
        std::string reason;
        if (auto device = TryCreate(kind, &reason)) return device;
        if (!reasons.empty()) reasons += "; ";
        reasons += std::string(ToString(kind)) + ": " + reason;
    }
    if (note) {
        note->clear();
        if (preference != DevicePreference::Cpu)
            *note = "no GPU device available (" + reasons + "); using the CPU device";
    }
    return std::make_unique<CpuDevice>();
}

} // namespace prism::device
