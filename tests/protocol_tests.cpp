#include "../src/protocol.h"

#include <algorithm>
#include <array>
#include <deque>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require(bool condition, const std::string& message) {
    if (!condition) { throw TestFailure(message); }
}

enum class Action { Write, Read, Wait };

struct Step {
    Action action;
    Bytes bytes;
    std::size_t requested = 0;
    std::uint32_t milliseconds = 0;
    std::uint64_t elapsed = 0;
    bool fail = false;
};

class FakeChannel final : public r8c::Channel {
public:
    std::deque<Step> script;
    std::vector<Bytes> writes;
    std::uint64_t clock = 0;

    void tx(Bytes bytes, bool fail = false) {
        script.push_back({Action::Write, std::move(bytes), 0, 0, 0, fail});
    }

    void rx(Bytes bytes, std::uint32_t timeout, bool fail = false,
            std::uint64_t elapsed = 0, std::size_t requested = 0) {
        const auto count = requested == 0 ? bytes.size() : requested;
        script.push_back({Action::Read, std::move(bytes), count, timeout, elapsed, fail});
    }

    void pause(std::uint32_t milliseconds) {
        script.push_back({Action::Wait, {}, 0, milliseconds});
    }

    void write(std::span<const std::uint8_t> bytes) override {
        const auto step = take(Action::Write);
        const Bytes actual(bytes.begin(), bytes.end());
        require(actual == step.bytes, "unexpected transmitted packet");
        writes.push_back(actual);
        if (step.fail) { throw std::runtime_error("synthetic channel write failure"); }
    }

    void read(std::span<std::uint8_t> bytes, std::uint32_t timeout) override {
        const auto step = take(Action::Read);
        require(bytes.size() == step.requested, "unexpected requested response length");
        require(timeout == step.milliseconds, "unexpected response timeout");
        require(step.bytes.size() <= bytes.size(), "invalid fake response size");
        std::copy(step.bytes.begin(), step.bytes.end(), bytes.begin());
        clock += step.elapsed;
        if (step.fail) { throw std::runtime_error("synthetic channel read failure"); }
    }

    void wait(std::uint32_t milliseconds) override {
        const auto step = take(Action::Wait);
        require(milliseconds == step.milliseconds, "unexpected wait duration");
        clock += milliseconds;
    }

    std::uint64_t now_ms() const override { return clock; }

    void consumed() const {
        require(script.empty(), "operation returned before consuming required transcript");
    }

private:
    Step take(Action action) {
        require(!script.empty(), "unexpected operation after transcript boundary");
        auto step = std::move(script.front());
        script.pop_front();
        require(step.action == action, "unexpected channel operation order");
        return step;
    }
};

const Bytes force_packet{0xF5, 0xDF, 0xFF, 0x00, 0x07, 0x41, 0x4C, 0x65, 0x52, 0x41, 0x53, 0x45};
const Bytes default_packet{0xF5, 0xDF, 0xFF, 0x00, 0x07, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr std::uint32_t total_bytes = 0x11000;

void append_status(FakeChannel& channel, std::uint8_t status = 0x80,
                   std::uint8_t authentication = 0x0C, std::uint32_t timeout = 2000,
                   bool fail = false, std::uint64_t elapsed = 0) {
    channel.tx({0x70});
    channel.rx({status, authentication}, timeout, fail, elapsed);
}

void append_sync(FakeChannel& channel, std::uint8_t reply = 0xB0, bool fail = false) {
    channel.pause(350);
    for (unsigned index = 0; index != 16; ++index) {
        channel.tx({0x00});
        channel.pause(25);
    }
    channel.tx({0xB0});
    channel.rx({reply}, 2000, fail);
}

void append_version(FakeChannel& channel, Bytes version = {'V', 'E', 'R', '.', '1', '.', '0', '0'}, bool fail = false) {
    channel.tx({0xFB});
    channel.rx(std::move(version), 2000, fail, 0, 8);
}

void append_force(FakeChannel& channel) {
    append_status(channel);
    channel.tx(force_packet);
}

void append_authorized(FakeChannel& channel) {
    append_force(channel);
    append_status(channel, 0x80, 0x0C, 60000);
    channel.tx(default_packet);
    append_status(channel);
}

void append_page(FakeChannel& channel, std::uint32_t index, Bytes data = Bytes(256, 0xFF), bool fail = false) {
    const auto address = 0x7000U + index * 256U;
    channel.tx({0xFF, static_cast<std::uint8_t>(address >> 8), static_cast<std::uint8_t>(address >> 16)});
    channel.rx(std::move(data), 3000, fail, 0, 256);
}

void append_pages(FakeChannel& channel, std::uint32_t count = 272) {
    for (std::uint32_t index = 0; index < count; ++index) { append_page(channel, index); }
}

void append_page_34g(FakeChannel& channel, std::uint32_t index, Bytes data = Bytes(256, 0xFF), bool fail = false) {
    const auto address = 0x3000U + index * 256U;
    channel.tx({0xFF, static_cast<std::uint8_t>(address >> 8), static_cast<std::uint8_t>(address >> 16)});
    channel.rx(std::move(data), 3000, fail, 0, 256);
}

void append_pages_34g(FakeChannel& channel, std::uint32_t count = 208) {
    for (std::uint32_t index = 0; index < count; ++index) { append_page_34g(channel, index); }
}

template<class Function>
void fails(Function&& action, const std::string& expected) {
    try {
        action();
    } catch (const TestFailure&) {
        throw;
    } catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, "unexpected failure reason");
        return;
    }
    throw TestFailure("operation falsely returned success");
}

void no_erase(const FakeChannel& channel) {
    require(std::none_of(channel.writes.begin(), channel.writes.end(), [](const Bytes& bytes) {
        return !bytes.empty() && (bytes[0] == 0xF5 || bytes[0] == 0xA7 || bytes[0] == 0x20 || bytes[0] == 0x40);
    }), "connect transmitted an ID, erase, or program command");
}

void one_force_no_other_erase(const FakeChannel& channel) {
    require(std::count(channel.writes.begin(), channel.writes.end(), force_packet) == 1, "forced erase was omitted or retried");
    require(std::none_of(channel.writes.begin(), channel.writes.end(), [](const Bytes& bytes) {
        return !bytes.empty() && (bytes[0] == 0xA7 || bytes[0] == 0x20 || bytes[0] == 0x40);
    }), "unexpected erase/program primitive");
}

std::size_t page_reads(const FakeChannel& channel) {
    return static_cast<std::size_t>(std::count_if(channel.writes.begin(), channel.writes.end(), [](const Bytes& bytes) {
        return bytes.size() == 3 && bytes[0] == 0xFF;
    }));
}

std::size_t passed = 0;
std::size_t failed = 0;

template<class Function>
void test(const std::string& name, Function&& function) {
    try {
        function();
        ++passed;
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
        ++failed;
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}

}

int main() {
    for (const auto authentication : {0x00, 0x04, 0x0C}) {
        test("connect exact transcript without destructive commands auth=" + std::to_string(authentication), [=] {
            FakeChannel channel;
            append_sync(channel);
            append_version(channel);
            append_status(channel, 0x80, static_cast<std::uint8_t>(authentication));
            const auto info = r8c::connect_boot(channel);
            channel.consumed();
            require(info.version == "VER.1.00" && info.status == 0x80 && info.authentication == authentication, "boot information mismatch");
            require(channel.clock == 750 && channel.writes.size() == 19, "synchronization spacing or count mismatch");
            no_erase(channel);
        });
    }

    test("bad handshake stops before version", [] {
        FakeChannel channel;
        append_sync(channel, 0x00);
        fails([&] { r8c::connect_boot(channel); }, "handshake");
        channel.consumed(); no_erase(channel);
    });

    for (std::size_t index = 0; index < 8; ++index) {
        test("invalid version byte " + std::to_string(index), [=] {
            FakeChannel channel;
            append_sync(channel);
            Bytes version{'V', 'E', 'R', '.', '1', '.', '0', '0'};
            version[index] = 0;
            append_version(channel, version);
            fails([&] { r8c::connect_boot(channel); }, "version");
            channel.consumed(); no_erase(channel);
        });
    }

    for (const auto status : {0x00, 0x90, 0xA0, 0xB0}) {
        test("connect rejects status " + std::to_string(status), [=] {
            FakeChannel channel;
            append_sync(channel); append_version(channel);
            append_status(channel, static_cast<std::uint8_t>(status));
            fails([&] { r8c::connect_boot(channel); }, status == 0 ? "busy" : "flash error");
            channel.consumed(); no_erase(channel);
        });
    }

    test("connect rejects unsupported authentication", [] {
        FakeChannel channel;
        append_sync(channel); append_version(channel); append_status(channel, 0x80, 0x08);
        fails([&] { r8c::connect_boot(channel); }, "authentication status");
        channel.consumed(); no_erase(channel);
    });

    for (const auto stage : {0, 1, 2}) {
        test("connect channel timeout stage=" + std::to_string(stage), [=] {
            FakeChannel channel;
            append_sync(channel, 0xB0, stage == 0);
            if (stage >= 1) { append_version(channel, {'V', 'E', 'R', '.', '1', '.', '0', '0'}, stage == 1); }
            if (stage == 2) { append_status(channel, 0x80, 0x0C, 2000, true); }
            fails([&] { r8c::connect_boot(channel); }, "channel read failure");
            channel.consumed(); no_erase(channel);
        });
    }

    test("device profiles match exact FDT memory areas", [] {
        const auto& mcu54g = r8c::device_profile(r8c::Device::R5F21548G);
        const auto& mcu34g = r8c::device_profile(r8c::Device::R5F21347G);
        require(std::string(mcu54g.name) == "R5F21548G" && mcu54g.first_address == 0x7000 && mcu54g.end_address == 0x18000, "54G profile mismatch");
        require(std::string(mcu34g.name) == "R5F21347G" && mcu34g.first_address == 0x3000 && mcu34g.end_address == 0x10000, "34G profile mismatch");
        require(mcu54g.end_address - mcu54g.first_address == 69632 && mcu34g.end_address - mcu34g.first_address == 53248, "profile size mismatch");
    });

    test("missing acknowledgement performs zero channel operations", [] {
        FakeChannel channel;
        bool progress = false;
        fails([&] { r8c::force_erase_and_verify(channel, r8c::Device::R5F21548G, false, [&](auto, auto) { progress = true; }); }, "acknowledged");
        require(channel.writes.empty() && channel.clock == 0 && !progress, "unacknowledged side effect");
        channel.consumed();
    });

    for (const auto response : std::array<std::array<std::uint8_t, 2>, 5>{{{0, 0x0C}, {0x90, 0x0C}, {0xA0, 0x0C}, {0xB0, 0x0C}, {0x80, 0x08}}}) {
        test("pre-erase status refusal " + std::to_string(response[0]) + "/" + std::to_string(response[1]), [=] {
            FakeChannel channel;
            append_status(channel, response[0], response[1]);
            const auto reason = response[0] == 0 ? "busy" : response[1] == 8 ? "authentication status" : "flash error";
            fails([&] { r8c::force_erase_and_verify(channel, r8c::Device::R5F21548G, true, {}); }, reason);
            channel.consumed(); no_erase(channel);
        });
    }

    test("denied forced ID cannot read pages or retry", [] {
        FakeChannel channel;
        append_force(channel); append_status(channel, 0x80, 0x04, 60000);
        fails([&] { r8c::force_erase_and_verify(channel, r8c::Device::R5F21548G, true, {}); }, "rejected");
        channel.consumed(); one_force_no_other_erase(channel);
        require(page_reads(channel) == 0 && std::count(channel.writes.begin(), channel.writes.end(), default_packet) == 0, "denial continued to authentication or read");
    });

    for (const auto response : std::array<std::array<std::uint8_t, 2>, 4>{{{0x90, 0x0C}, {0xA0, 0x0C}, {0xB0, 0x0C}, {0x80, 0x08}}}) {
        test("erase status failure stops immediately " + std::to_string(response[0]) + "/" + std::to_string(response[1]), [=] {
            FakeChannel channel;
            append_force(channel); append_status(channel, response[0], response[1], 60000);
            fails([&] { r8c::force_erase_and_verify(channel, r8c::Device::R5F21548G, true, {}); }, response[1] == 8 ? "authentication status" : "flash error");
            channel.consumed(); one_force_no_other_erase(channel);
            require(page_reads(channel) == 0, "failed erase proceeded to reads");
        });
    }

    test("busy erase uses bounded deadline and no retry", [] {
        FakeChannel channel;
        append_force(channel);
        append_status(channel, 0, 0, 60000, false, 59800);
        channel.pause(100);
        append_status(channel, 0, 0, 100);
        channel.pause(100);
        fails([&] { r8c::force_erase_and_verify(channel, r8c::Device::R5F21548G, true, {}); }, "Timed out");
        channel.consumed(); one_force_no_other_erase(channel);
        require(page_reads(channel) == 0, "timeout proceeded to page read");
    });

    for (const auto elapsed : {60000U, 60001U}) {
        test("late ready erase response must fail at " + std::to_string(elapsed), [=] {
            FakeChannel channel;
            append_force(channel); append_status(channel, 0x80, 0x0C, 60000, false, elapsed);
            fails([&] { r8c::force_erase_and_verify(channel, r8c::Device::R5F21548G, true, {}); }, "timeout");
            channel.consumed(); one_force_no_other_erase(channel);
            require(page_reads(channel) == 0, "late ready response proceeded to verification");
        });
    }

    for (const auto response : std::array<std::array<std::uint8_t, 2>, 3>{{{0x88, 0x0C}, {0x80, 0x0E}, {0x80, 0x8C}}}) {
        for (const auto stage : {0, 1, 2, 3, 4}) {
            test("corroborated flash error bits at stage " + std::to_string(stage) + "/" + std::to_string(response[0]) + "/" + std::to_string(response[1]), [=] {
                FakeChannel channel;
                if (stage == 0) { append_sync(channel); append_version(channel); }
                if (stage >= 2) { append_force(channel); }
                if (stage >= 3) {
                    append_status(channel, 0x80, 0x0C, 60000);
                    channel.tx(default_packet);
                }
                if (stage == 4) { append_status(channel); append_pages(channel); }
                append_status(channel, response[0], response[1], stage == 2 ? 60000 : 2000);
                fails([&] {
                    if (stage == 0) { r8c::connect_boot(channel); }
                    else { r8c::force_erase_and_verify(channel, r8c::Device::R5F21548G, true, {}); }
                }, "flash error");
                channel.consumed();
                if (stage < 2) { no_erase(channel); } else { one_force_no_other_erase(channel); }
            });
        }
    }

    test("completed status with high-bit completion flag remains accepted", [] {
        FakeChannel channel;
        append_sync(channel); append_version(channel); append_status(channel, 0x80, 0x9C);
        const auto info = r8c::connect_boot(channel);
        require(info.authentication == 0x9C, "valid extended status was lost");
        channel.consumed(); no_erase(channel);
    });

    for (const auto response : std::array<std::array<std::uint8_t, 2>, 6>{{{0, 0x0C}, {0x80, 0}, {0x80, 4}, {0x80, 8}, {0x90, 0x0C}, {0xA0, 0x0C}}}) {
        test("default ID refusal cannot establish success " + std::to_string(response[0]) + "/" + std::to_string(response[1]), [=] {
            FakeChannel channel;
            append_force(channel); append_status(channel, 0x80, 0x0C, 60000);
            channel.tx(default_packet); append_status(channel, response[0], response[1]);
            const auto reason = (response[0] & 0x30) != 0 ? "flash error" : response[1] == 8 ? "authentication status" : "not accepted";
            fails([&] { r8c::force_erase_and_verify(channel, r8c::Device::R5F21548G, true, {}); }, reason);
            channel.consumed(); one_force_no_other_erase(channel);
            require(page_reads(channel) == 0, "default ID refusal proceeded to read");
        });
    }

    for (const auto busy_polls : {0, 2}) {
        test("success requires all 272 pages and final ready status polls=" + std::to_string(busy_polls), [=] {
            FakeChannel channel;
            append_force(channel);
            for (int index = 0; index < busy_polls; ++index) {
                append_status(channel, 0, 0, 60000U - static_cast<std::uint32_t>(index) * 100U);
                channel.pause(100);
            }
            append_status(channel, 0x80, 0x0C, 60000U - static_cast<std::uint32_t>(busy_polls) * 100U);
            channel.tx(default_packet); append_status(channel);
            append_pages(channel); append_status(channel);
            std::vector<std::pair<std::uint32_t, std::uint32_t>> progress;
            r8c::force_erase_and_verify(channel, r8c::Device::R5F21548G, true, [&](auto verified, auto total) { progress.emplace_back(verified, total); });
            channel.consumed(); one_force_no_other_erase(channel);
            require(page_reads(channel) == 272 && progress.size() == 273, "incomplete page coverage/progress");
            require(std::count(channel.writes.begin(), channel.writes.end(), default_packet) == 1, "default ID must be checked once");
            for (std::uint32_t index = 0; index <= 272; ++index) {
                require(progress[index] == std::pair{index * 256U, total_bytes}, "progress does not match verified coverage");
            }
            std::vector<Bytes> requests;
            for (const auto& packet : channel.writes) { if (packet.size() == 3) { requests.push_back(packet); } }
            require(requests.front() == Bytes{0xFF, 0x70, 0} && requests[143] == Bytes{0xFF, 0xFF, 0} && requests[144] == Bytes{0xFF, 0, 1} && requests.back() == Bytes{0xFF, 0x7F, 1}, "page address rollover/endpoints mismatch");
        });
    }

    for (const auto page_index : {0U, 16U, 143U, 144U, 271U}) {
        for (const auto byte_index : {0U, 127U, 255U}) {
            test("nonblank page stops at " + std::to_string(page_index) + "/" + std::to_string(byte_index), [=] {
                FakeChannel channel;
                append_authorized(channel); append_pages(channel, page_index);
                Bytes page(256, 0xFF); page[byte_index] = 0;
                append_page(channel, page_index, page);
                std::uint32_t verified = 0;
                fails([&] { r8c::force_erase_and_verify(channel, r8c::Device::R5F21548G, true, [&](auto count, auto) { verified = count; }); }, "Blank verification failed");
                channel.consumed(); one_force_no_other_erase(channel);
                require(page_reads(channel) == page_index + 1 && verified == page_index * 256, "failed page advanced verification");
            });
        }
    }

    test("34G success verifies exactly 208 pages from 3000 through FF00", [] {
        FakeChannel channel;
        append_authorized(channel); append_pages_34g(channel); append_status(channel);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> progress;
        r8c::force_erase_and_verify(channel, r8c::Device::R5F21347G, true, [&](auto verified, auto total) { progress.emplace_back(verified, total); });
        channel.consumed(); one_force_no_other_erase(channel);
        require(page_reads(channel) == 208 && progress.size() == 209, "34G page coverage/progress mismatch");
        for (std::uint32_t index = 0; index <= 208; ++index) {
            require(progress[index] == std::pair{index * 256U, 0xD000U}, "34G verified total mismatch");
        }
        std::vector<Bytes> requests;
        for (const auto& packet : channel.writes) { if (packet.size() == 3) { requests.push_back(packet); } }
        require(requests.size() == 208 && requests.front() == Bytes{0xFF, 0x30, 0} && requests[15] == Bytes{0xFF, 0x3F, 0} &&
            requests[16] == Bytes{0xFF, 0x40, 0} && requests.back() == Bytes{0xFF, 0xFF, 0}, "34G read outside its memory areas");
    });

    test("34G nonblank final page cannot advance to 10000", [] {
        FakeChannel channel;
        append_authorized(channel); append_pages_34g(channel, 207);
        Bytes page(256, 0xFF); page[255] = 0;
        append_page_34g(channel, 207, page);
        std::uint32_t verified = 0;
        fails([&] { r8c::force_erase_and_verify(channel, r8c::Device::R5F21347G, true, [&](auto count, auto) { verified = count; }); }, "Blank verification failed");
        channel.consumed(); one_force_no_other_erase(channel);
        require(page_reads(channel) == 208 && verified == 207 * 256, "34G failure advanced verification");
    });

    for (const auto response : std::array<std::array<std::uint8_t, 2>, 6>{{{0, 0x0C}, {0x80, 0}, {0x80, 4}, {0x80, 8}, {0x90, 0x0C}, {0xA0, 0x0C}}}) {
        test("final status is mandatory " + std::to_string(response[0]) + "/" + std::to_string(response[1]), [=] {
            FakeChannel channel;
            append_authorized(channel); append_pages(channel); append_status(channel, response[0], response[1]);
            const auto reason = (response[0] & 0x30) != 0 ? "flash error" : response[1] == 8 ? "authentication status" : "Final status";
            fails([&] { r8c::force_erase_and_verify(channel, r8c::Device::R5F21548G, true, {}); }, reason);
            channel.consumed(); one_force_no_other_erase(channel);
            require(page_reads(channel) == 272, "final-status test did not cover all pages");
        });
    }

    for (const auto page_index : {0U, 1U, 271U}) {
        for (const auto throws : {false, true}) {
            test("partial page cannot reuse old blank bytes " + std::to_string(page_index) + "/" + std::to_string(throws), [=] {
                FakeChannel channel;
                append_authorized(channel); append_pages(channel, page_index);
                append_page(channel, page_index, Bytes(128, 0xFF), throws);
                fails([&] { r8c::force_erase_and_verify(channel, r8c::Device::R5F21548G, true, {}); }, throws ? "channel read failure" : "Blank verification failed");
                channel.consumed(); one_force_no_other_erase(channel);
            });
        }
    }

    for (const auto stage : {0, 1, 2, 3}) {
        test("erase status channel failure has no false success stage=" + std::to_string(stage), [=] {
            FakeChannel channel;
            if (stage == 0) { append_status(channel, 0x80, 0x0C, 2000, true); }
            if (stage == 1) { append_force(channel); append_status(channel, 0x80, 0x0C, 60000, true); }
            if (stage == 2) {
                append_force(channel); append_status(channel, 0x80, 0x0C, 60000);
                channel.tx(default_packet); append_status(channel, 0x80, 0x0C, 2000, true);
            }
            if (stage == 3) { append_authorized(channel); append_pages(channel); append_status(channel, 0x80, 0x0C, 2000, true); }
            fails([&] { r8c::force_erase_and_verify(channel, r8c::Device::R5F21548G, true, {}); }, "channel read failure");
            channel.consumed();
            if (stage == 0) { no_erase(channel); } else { one_force_no_other_erase(channel); }
        });
    }

    test("force write exception never retries", [] {
        FakeChannel channel;
        append_status(channel); channel.tx(force_packet, true);
        fails([&] { r8c::force_erase_and_verify(channel, r8c::Device::R5F21548G, true, {}); }, "channel write failure");
        channel.consumed(); one_force_no_other_erase(channel);
    });

    std::cout << passed << " passed, " << failed << " failed; fake channel only, no hardware\n";
    return failed == 0 ? 0 : 1;
}
