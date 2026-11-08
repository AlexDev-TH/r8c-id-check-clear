#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>

namespace r8c {

class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Channel {
public:
    virtual ~Channel() = default;
    virtual void write(std::span<const std::uint8_t> bytes) = 0;
    virtual void read(std::span<std::uint8_t> bytes, std::uint32_t timeout_ms) = 0;
    virtual void wait(std::uint32_t milliseconds) = 0;
    virtual std::uint64_t now_ms() const = 0;
};

struct BootInfo {
    std::string version;
    std::uint8_t status = 0;
    std::uint8_t authentication = 0;
};

enum class Device { R5F21548G, R5F21347G };

struct DeviceProfile {
    const char* name;
    std::uint32_t first_address;
    std::uint32_t end_address;
};

const DeviceProfile& device_profile(Device device);

using Progress = std::function<void(std::uint32_t verified, std::uint32_t total)>;

BootInfo connect_boot(Channel& channel);
void force_erase_and_verify(Channel& channel, Device device, bool acknowledged, const Progress& progress);

}
