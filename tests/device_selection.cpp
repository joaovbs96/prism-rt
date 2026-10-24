// Device selection: parsing, availability and the CPU fallback.
#include "../src/device/device.h"

#include <iostream>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main()
{
    using namespace prism::device;
    DevicePreference preference = DevicePreference::Cpu;
    check(ParseDevicePreference("auto", &preference) && preference == DevicePreference::Auto,
          "parse auto");
    check(ParseDevicePreference("METAL", &preference) && preference == DevicePreference::Metal,
          "parse is case-insensitive");
    check(ParseDevicePreference("vulkan", &preference) && preference == DevicePreference::Vulkan,
          "parse vulkan");
    check(!ParseDevicePreference("cuda", &preference), "reject unknown devices");
    check(RemovedDeviceMessage("cuda").empty(), "an unknown device was never removed");
    check(!ParseDevicePreference("cpu-reference", &preference) &&
          RemovedDeviceMessage("CPU-Reference") == "the cpu-reference device was removed; use cpu",
          "the removed cpu-reference device is rejected with a plain message");

    const auto devices = AvailableDevices();
    check(!devices.empty() && devices.back().kind == DeviceKind::Cpu,
          "the CPU device is always available and listed last");

    std::string note;
    auto cpu = CreateDevice(DevicePreference::Cpu, &note);
    check(cpu && cpu->Info().kind == DeviceKind::Cpu && note.empty(),
          "an explicit CPU request creates the CPU device without a note");

    // Auto always yields a device: a GPU when available, else the CPU with a note.
    auto automatic = CreateDevice(DevicePreference::Auto, &note);
    check(automatic != nullptr, "auto creates a device");
    if (automatic && automatic->Info().kind == DeviceKind::Cpu)
        check(!note.empty(), "falling back to the CPU explains why");
    else if (automatic)
        check(devices.size() > 1 && devices.front().kind == automatic->Info().kind,
              "auto picks the first available GPU device");

    for (DevicePreference gpu : {DevicePreference::Metal, DevicePreference::Vulkan}) {
        auto device = CreateDevice(gpu, &note);
        check(device != nullptr, std::string("requesting ") + ToString(gpu) + " yields a device");
        if (device && device->Info().kind == DeviceKind::Cpu)
            check(!note.empty(), std::string("unavailable ") + ToString(gpu) + " falls back with a note");
    }

    if (failures) return 1;
    std::cout << "device selection: all checks passed\n";
    return 0;
}
