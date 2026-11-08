#include "protocol.h"

#include <algorithm>

namespace r8c {
namespace {

constexpr DeviceProfile r5f21548g{"R5F21548G", 0x7000, 0x18000};
constexpr DeviceProfile r5f21347g{"R5F21347G", 0x3000, 0x10000};
constexpr std::array<std::uint8_t, 12> force_id = {
    0xF5, 0xDF, 0xFF, 0x00, 0x07, 0x41, 0x4C, 0x65, 0x52, 0x41, 0x53, 0x45
};
constexpr std::array<std::uint8_t, 12> blank_id = {
    0xF5, 0xDF, 0xFF, 0x00, 0x07, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

void command(Channel& channel, std::uint8_t value) {
    channel.write(std::span<const std::uint8_t>(&value, 1));
}

std::array<std::uint8_t, 2> status(Channel& channel, std::uint32_t timeout_ms) {
    command(channel, 0x70);
    std::array<std::uint8_t, 2> result{};
    channel.read(result, timeout_ms);
    return result;
}

void check_errors(const std::array<std::uint8_t, 2>& result) {
    if ((result[0] & 0x38) != 0 || (result[1] & 0x02) != 0 ||
        ((result[1] & 0x80) != 0 && (result[1] & 0x10) == 0)) {
        throw ProtocolError("The MCU reports a flash error. Erasure is not verified; reset and inspect the device with your programmer.");
    }
    if ((result[1] & 0x0C) == 0x08) {
        throw ProtocolError("The MCU returned an unsupported authentication status. No success can be established.");
    }
}

}

BootInfo connect_boot(Channel& channel) {
    channel.wait(350);
    for (unsigned index = 0; index < 16; ++index) {
        command(channel, 0);
        channel.wait(25);
    }
    command(channel, 0xB0);
    std::array<std::uint8_t, 1> reply{};
    channel.read(reply, 2000);
    if (reply[0] != 0xB0) {
        throw ProtocolError("No valid boot handshake. Check the direct UART wiring, MODE and reset, then reconnect.");
    }
    command(channel, 0xFB);
    std::array<std::uint8_t, 8> version{};
    channel.read(version, 2000);
    const auto digit = [](std::uint8_t value) { return value >= '0' && value <= '9'; };
    if (version[0] != 'V' || version[1] != 'E' || version[2] != 'R' || version[3] != '.' ||
        !digit(version[4]) || version[5] != '.' || !digit(version[6]) || !digit(version[7])) {
        throw ProtocolError("Unsupported boot version reply. This connection is not eligible for an erase request.");
    }
    const auto result = status(channel, 2000);
    check_errors(result);
    if ((result[0] & 0x80) == 0) {
        throw ProtocolError("The MCU is busy. Reset into the serial programming mode before connecting again.");
    }
    return {std::string(version.begin(), version.end()), result[0], result[1]};
}

const DeviceProfile& device_profile(Device device) {
    switch (device) {
    case Device::R5F21548G: return r5f21548g;
    case Device::R5F21347G: return r5f21347g;
    }
    throw ProtocolError("Unsupported MCU profile.");
}

void force_erase_and_verify(Channel& channel, Device device, bool acknowledged, const Progress& progress) {
    if (!acknowledged) {
        throw ProtocolError("Erasing all program and data flash must be acknowledged before any erase command is sent.");
    }
    const auto& profile = device_profile(device);
    const auto before = status(channel, 2000);
    check_errors(before);
    if ((before[0] & 0x80) == 0) {
        throw ProtocolError("The MCU is busy. No erase request was sent.");
    }
    channel.write(force_id);
    const auto deadline = channel.now_ms() + 60000;
    std::array<std::uint8_t, 2> result{};
    for (;;) {
        const auto now = channel.now_ms();
        if (now >= deadline) {
            throw ProtocolError("Timed out waiting for erasure. The device state is unknown; no erase will be retried automatically.");
        }
        result = status(channel, static_cast<std::uint32_t>(deadline - now));
        if (channel.now_ms() >= deadline) {
            throw ProtocolError("The erase response arrived after the timeout. The device state is not verified.");
        }
        check_errors(result);
        if ((result[0] & 0x80) != 0) {
            break;
        }
        channel.wait(100);
    }
    if ((result[1] & 0x0C) == 0x04) {
        throw ProtocolError("The MCU rejected the forced-erase ID. ROM Code Protect may prohibit this operation. This tool cannot bypass that protection.");
    }
    channel.write(blank_id);
    const auto blank_authentication = status(channel, 2000);
    check_errors(blank_authentication);
    if ((blank_authentication[0] & 0x80) == 0 || (blank_authentication[1] & 0x0C) != 0x0C) {
        throw ProtocolError("The erased ID was not accepted. Clearing the ID is not verified.");
    }
    if (progress) { progress(0, profile.end_address - profile.first_address); }
    std::array<std::uint8_t, 256> page{};
    for (std::uint32_t address = profile.first_address; address < profile.end_address; address += 256) {
        const std::array<std::uint8_t, 3> request = {
            0xFF, static_cast<std::uint8_t>(address >> 8), static_cast<std::uint8_t>(address >> 16)
        };
        channel.write(request);
        channel.read(page, 3000);
        const bool blank = std::all_of(page.begin(), page.end(), [](std::uint8_t value) { return value == 0xFF; });
        std::fill(page.begin(), page.end(), std::uint8_t{0});
        if (!blank) {
            throw ProtocolError("Blank verification failed. At least one program/data flash byte remains programmed; ID clear is not confirmed.");
        }
        if (progress) { progress(address + 256 - profile.first_address, profile.end_address - profile.first_address); }
    }
    const auto final_status = status(channel, 2000);
    check_errors(final_status);
    if ((final_status[0] & 0x80) == 0 || (final_status[1] & 0x0C) != 0x0C) {
        throw ProtocolError("Final status did not confirm a ready, authenticated device. Erasure is not verified.");
    }
}

}
