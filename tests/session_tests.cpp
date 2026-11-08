#include "../src/serial_job.h"

#include <iostream>
#include <stdexcept>

int main() {
    try {
        for (const auto* port : {L"", L"COM", L"COM0", L"COM01", L"COM-1", L"COM1\n", L"COM1/other", L"com14", L"\\\\.\\COM14"}) {
            if (r8c::valid_port(port)) { throw std::runtime_error("Invalid port accepted"); }
        }
        for (const auto* port : {L"COM1", L"COM14", L"COM256"}) {
            if (!r8c::valid_port(port)) { throw std::runtime_error("Valid port rejected"); }
        }
        r8c::Session session;
        const auto before_connection = session.clear_id(true, {});
        if (before_connection.success || before_connection.connected || before_connection.erase_verified) {
            throw std::runtime_error("Clear without connection accepted");
        }
        for (const auto device : {r8c::Device::R5F21548G, r8c::Device::R5F21347G}) {
            const auto invalid = session.connect(L"not-a-port", device, {});
            if (invalid.success || invalid.connected || invalid.erase_verified) {
                throw std::runtime_error("Invalid connect accepted");
            }
        }
        const auto disconnected = session.disconnect();
        if (!disconnected.success || disconnected.connected || disconnected.erase_verified) {
            throw std::runtime_error("Disconnect state invalid");
        }
        std::cout << "Session boundary checks passed. No COM port opened.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
