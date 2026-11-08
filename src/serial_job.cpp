#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include "serial_job.h"
#include "protocol.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace r8c {
namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) { return {}; }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size == 0) { return L"Unexpected communication failure."; }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) { return {}; }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size == 0) { throw ProtocolError("Cannot encode the log text."); }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

[[noreturn]] void serial_error(const char* action) {
    const auto code = GetLastError();
    throw ProtocolError(std::string(action) + " (Windows error " + std::to_string(code) + "). Close other programs using this COM port and check the adapter.");
}

class Serial final : public Channel {
public:
    explicit Serial(const std::wstring& port) {
        const std::wstring name = L"\\\\.\\" + port;
        handle_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) { serial_error("Cannot open serial port"); }
        try {
            DCB state{};
            state.DCBlength = sizeof(state);
            if (!GetCommState(handle_, &state)) { serial_error("Cannot read serial configuration"); }
            state.BaudRate = 9600;
            state.ByteSize = 8;
            state.Parity = NOPARITY;
            state.StopBits = ONESTOPBIT;
            state.fBinary = TRUE;
            state.fParity = FALSE;
            state.fOutxCtsFlow = FALSE;
            state.fOutxDsrFlow = FALSE;
            state.fDtrControl = DTR_CONTROL_DISABLE;
            state.fDsrSensitivity = FALSE;
            state.fTXContinueOnXoff = TRUE;
            state.fOutX = FALSE;
            state.fInX = FALSE;
            state.fErrorChar = FALSE;
            state.fNull = FALSE;
            state.fRtsControl = RTS_CONTROL_DISABLE;
            state.fAbortOnError = FALSE;
            if (!SetCommState(handle_, &state)) { serial_error("Cannot configure serial port"); }
            COMMTIMEOUTS timeouts{};
            timeouts.ReadIntervalTimeout = MAXDWORD;
            timeouts.ReadTotalTimeoutConstant = 100;
            timeouts.WriteTotalTimeoutConstant = 2000;
            if (!SetCommTimeouts(handle_, &timeouts) || !SetupComm(handle_, 4096, 4096) ||
                !PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR)) {
                serial_error("Cannot initialize serial buffers");
            }
            DWORD errors = 0;
            COMSTAT status{};
            if (!ClearCommError(handle_, &errors, &status)) { serial_error("Cannot read serial status"); }
        } catch (...) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw;
        }
    }

    ~Serial() override {
        if (handle_ != INVALID_HANDLE_VALUE) { CloseHandle(handle_); }
    }

    void write(std::span<const std::uint8_t> bytes) override {
        for (const auto byte : bytes) {
            DWORD written = 0;
            if (!WriteFile(handle_, &byte, 1, &written, nullptr)) { serial_error("Serial write failed"); }
            if (written != 1) { throw ProtocolError("Serial write timed out. The device state may be unknown; reset before reconnecting."); }
            check_line();
            Sleep(2);
        }
    }

    void read(std::span<std::uint8_t> bytes, std::uint32_t timeout_ms) override {
        const auto deadline = GetTickCount64() + timeout_ms;
        std::size_t received = 0;
        while (received < bytes.size()) {
            if (GetTickCount64() >= deadline) {
                throw ProtocolError("Serial response timed out or was incomplete. No operation will be retried automatically; the device state is unverified.");
            }
            DWORD count = 0;
            if (!ReadFile(handle_, bytes.data() + received, static_cast<DWORD>(bytes.size() - received), &count, nullptr)) {
                serial_error("Serial read failed");
            }
            if (GetTickCount64() >= deadline) {
                throw ProtocolError("The serial response exceeded its deadline. Device state is not verified.");
            }
            check_line();
            received += count;
        }
    }

    void wait(std::uint32_t milliseconds) override { Sleep(milliseconds); }
    std::uint64_t now_ms() const override { return GetTickCount64(); }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;

    void check_line() {
        DWORD errors = 0;
        COMSTAT state{};
        if (!ClearCommError(handle_, &errors, &state)) { serial_error("Cannot read serial errors"); }
        if ((errors & (CE_BREAK | CE_FRAME | CE_OVERRUN | CE_RXOVER | CE_RXPARITY)) != 0) {
            throw ProtocolError("UART framing, parity, break or receive-buffer error. Check wiring and reset; operation is not verified.");
        }
    }
};

std::filesystem::path executable_directory() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size()) { throw ProtocolError("Cannot determine the application directory."); }
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path new_log_directory() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) { throw ProtocolError("Cannot create a unique session identifier."); }
    wchar_t text[40]{};
    if (StringFromGUID2(id, text, 40) == 0) { throw ProtocolError("Cannot format the session identifier."); }
    const auto root = executable_directory() / L"logs";
    std::filesystem::create_directories(root);
    const auto directory = root / text;
    if (!std::filesystem::create_directory(directory)) { throw ProtocolError("Cannot create a new session log directory."); }
    return directory;
}

}

struct Session::Impl {
    std::unique_ptr<Serial> channel;
    std::filesystem::path log_directory;
    std::ofstream logfile;
    std::wstring version;
    Device device = Device::R5F21548G;

    void record(const std::wstring& message, const LogCallback& callback = {}) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t timestamp[40]{};
        swprintf_s(timestamp, L"%04u-%02u-%02u %02u:%02u:%02u  ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        if (logfile.is_open()) {
            logfile << utf8(timestamp + message) << '\n';
            logfile.flush();
        }
        if (callback) { callback(message); }
    }

    JobResult result(bool success, bool verified, const std::wstring& message) const {
        return {success, channel != nullptr, verified, message, version, log_directory.wstring()};
    }
};

bool valid_port(const std::wstring& port) {
    if (port.size() < 4 || port.size() > 8 || port.substr(0, 3) != L"COM" || port[3] == L'0') { return false; }
    return std::all_of(port.begin() + 3, port.end(), [](wchar_t value) { return value >= L'0' && value <= L'9'; });
}

std::vector<std::wstring> serial_ports() {
    std::vector<wchar_t> names(65536);
    const auto length = QueryDosDeviceW(nullptr, names.data(), static_cast<DWORD>(names.size()));
    if (length == 0) { return {}; }
    std::vector<std::wstring> ports;
    const wchar_t* cursor = names.data();
    const wchar_t* end = names.data() + length;
    while (cursor < end && *cursor != L'\0') {
        const std::wstring name(cursor);
        if (valid_port(name)) { ports.push_back(name); }
        cursor += name.size() + 1;
    }
    std::sort(ports.begin(), ports.end(), [](const auto& left, const auto& right) {
        if (left.size() != right.size()) { return left.size() < right.size(); }
        return left < right;
    });
    return ports;
}

Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() = default;

JobResult Session::connect(const std::wstring& port, Device device, const LogCallback& log) {
    if (impl_->channel) { return impl_->result(false, false, L"Disconnect the existing session before connecting again."); }
    if (!valid_port(port)) { return impl_->result(false, false, L"Select a valid direct USB-to-UART COM port."); }
    try {
        const auto& profile = device_profile(device);
        impl_->logfile.close();
        impl_->logfile.clear();
        impl_->version.clear();
        impl_->log_directory = new_log_directory();
        impl_->logfile.open(impl_->log_directory / L"session.log", std::ios::binary | std::ios::out);
        if (!impl_->logfile) { throw ProtocolError("Cannot create the session log; no device was opened."); }
        impl_->record(L"Direct UART session: " + port + L". Profile " + widen(profile.name) + L"; hardware qualification pending.", log);
        impl_->record(L"Opening the port with DTR/RTS disabled. MODE and RESET must be controlled manually on the known programming harness.", log);
        auto channel = std::make_unique<Serial>(port);
        const auto boot = connect_boot(*channel);
        impl_->version = widen(boot.version);
        impl_->device = device;
        impl_->channel = std::move(channel);
        const auto message = L"Connected: " + impl_->version + L". Boot reply does not identify the chip or prove that erasure is permitted. No ID/erase command sent.";
        impl_->record(message, log);
        return impl_->result(true, false, message);
    } catch (const std::exception& error) {
        impl_->channel.reset();
        const auto message = widen(error.what());
        impl_->record(message, log);
        return impl_->result(false, false, message);
    }
}

JobResult Session::clear_id(bool erase_acknowledged, const LogCallback& log, const Progress& progress) {
    if (!impl_->channel) { return impl_->result(false, false, L"Connect to the MCU boot interface first."); }
    if (!erase_acknowledged) { return impl_->result(false, false, L"Confirm the exact MCU and loss of all program/data flash first. No erase request sent."); }
    try {
        const auto& profile = device_profile(impl_->device);
        impl_->record(L"Requesting the manufacturer's conditional forced erase once. All program and data flash may be lost.", log);
        force_erase_and_verify(*impl_->channel, impl_->device, true, [this, &log, &progress](std::uint32_t verified, std::uint32_t total) {
            if (verified == 0 || verified == total || verified % 4096 == 0) {
                impl_->record(L"Blank verification: " + std::to_wstring(verified) + L" / " + std::to_wstring(total) + L" bytes.", log);
            }
            if (progress) { progress(verified, total); }
        });
        impl_->channel.reset();
        const std::wstring message = L"Erased ID accepted and all " + std::to_wstring(profile.end_address - profile.first_address) +
            L" program/data flash bytes verified blank for " + widen(profile.name) +
            L". Port released. Reset into programming mode, then use your programmer to restore a valid firmware image.";
        impl_->record(message, log);
        return impl_->result(true, true, message);
    } catch (const std::exception& error) {
        impl_->channel.reset();
        const auto message = widen(error.what());
        impl_->record(message + L" Port released; no automatic retry.", log);
        return impl_->result(false, false, message);
    }
}

JobResult Session::disconnect() {
    impl_->channel.reset();
    const std::wstring message = L"COM port released. No reset, erase or programming command was sent by Disconnect.";
    impl_->record(message);
    return impl_->result(true, false, message);
}

}
