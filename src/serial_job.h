#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "protocol.h"

namespace r8c {

struct JobResult {
    bool success = false;
    bool connected = false;
    bool erase_verified = false;
    std::wstring message;
    std::wstring boot_version;
    std::wstring log_directory;
};

using LogCallback = std::function<void(const std::wstring&)>;

std::vector<std::wstring> serial_ports();
bool valid_port(const std::wstring& port);

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    JobResult connect(const std::wstring& port, Device device, const LogCallback& log);
    JobResult clear_id(bool erase_acknowledged, const LogCallback& log, const Progress& progress = {});
    JobResult disconnect();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
