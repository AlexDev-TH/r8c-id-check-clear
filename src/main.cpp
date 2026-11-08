#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "serial_job.h"

namespace {

constexpr wchar_t app_title[] = L"R8C ID Check Clear By AlexDev-TH";
constexpr UINT_PTR poll_timer = 1;
constexpr int id_port = 101;
constexpr int id_refresh = 102;
constexpr int id_help = 103;
constexpr int id_connect = 104;
constexpr int id_clear = 105;
constexpr int id_log = 107;
constexpr int id_device = 108;

enum class Operation { Connect, Clear, Disconnect };

struct App {
    HWND window = nullptr;
    HWND device_label = nullptr;
    HWND device = nullptr;
    HWND port_label = nullptr;
    HWND port = nullptr;
    HWND refresh = nullptr;
    HWND connect = nullptr;
    HWND clear = nullptr;
    HWND view_log = nullptr;
    HWND help = nullptr;
    HWND status = nullptr;
    HWND progress = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    bool busy = false;
    bool connected = false;
    bool close_after_disconnect = false;
    Operation operation = Operation::Connect;
    std::wstring log_directory;
    r8c::Session session;
    std::thread worker;
    std::mutex mutex;
    std::optional<r8c::JobResult> result;
    std::uint32_t prog_done = 0;    // guarded by mutex (written by worker)
    std::uint32_t prog_total = 0;   // guarded by mutex
    std::uint64_t prog_start_ms = 0; // UI thread only
};

int scaled(const App& app, int value) {
    return MulDiv(value, static_cast<int>(app.dpi), 96);
}

std::wstring window_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

std::wstring fmt1(double value) {
    wchar_t buffer[32];
    swprintf_s(buffer, L"%.1f", value);
    return buffer;
}

std::wstring fmt_eta(double seconds) {
    if (seconds < 0.0) { seconds = 0.0; }
    const int total = static_cast<int>(seconds + 0.5);
    wchar_t buffer[32];
    if (total >= 60) { swprintf_s(buffer, L"%d:%02d นาที", total / 60, total % 60); }
    else { swprintf_s(buffer, L"%d วิ", total); }
    return buffer;
}

std::optional<r8c::Device> selected_device(const App& app) {
    switch (SendMessageW(app.device, CB_GETCURSEL, 0, 0)) {
    case 0: return r8c::Device::R5F21548G;
    case 1: return r8c::Device::R5F21347G;
    default: return std::nullopt;
    }
}

void update_controls(App& app) {
    const bool idle = !app.busy;
    EnableWindow(app.device, idle && !app.connected);
    EnableWindow(app.port, idle && !app.connected);
    EnableWindow(app.refresh, idle && !app.connected);
    EnableWindow(app.connect, idle && (app.connected ||
        (selected_device(app) && r8c::valid_port(window_text(app.port)))));
    SetWindowTextW(app.connect, app.connected ? L"Disconnect" : L"Connect");
    EnableWindow(app.clear, idle && app.connected);
    std::error_code error;
    EnableWindow(app.view_log,
                 !app.log_directory.empty() && std::filesystem::is_regular_file(
                     std::filesystem::path(app.log_directory) / L"session.log", error));
    const bool determinate = app.busy && app.operation == Operation::Clear;
    const LONG_PTR style = GetWindowLongPtrW(app.progress, GWL_STYLE);
    const bool has_marquee = (style & PBS_MARQUEE) != 0;
    if (determinate) {
        if (has_marquee) {
            SendMessageW(app.progress, PBM_SETMARQUEE, FALSE, 0);
            SetWindowLongPtrW(app.progress, GWL_STYLE, style & ~static_cast<LONG_PTR>(PBS_MARQUEE));
        }
    } else {
        if (!has_marquee) {
            SetWindowLongPtrW(app.progress, GWL_STYLE, style | PBS_MARQUEE);
        }
        SendMessageW(app.progress, PBM_SETMARQUEE, app.busy ? TRUE : FALSE, 35);
    }
    ShowWindow(app.progress, app.busy ? SW_SHOW : SW_HIDE);
}

void refresh_ports(App& app) {
    const auto previous = window_text(app.port);
    SendMessageW(app.port, CB_RESETCONTENT, 0, 0);
    const auto ports = r8c::serial_ports();
    int selected = -1;
    for (std::size_t index = 0; index < ports.size(); ++index) {
        const auto added = SendMessageW(app.port, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ports[index].c_str()));
        if (added >= 0 && ports[index] == previous) {
            selected = static_cast<int>(added);
        }
    }
    SendMessageW(app.port, CB_SETCURSEL, selected, 0);
    if (ports.empty()) {
        SetWindowTextW(app.status, L"ไม่พบ COM");
    } else if (!app.connected) {
        SetWindowTextW(app.status, L"ยังไม่เชื่อมต่อ");
    }
    update_controls(app);
}

void create_fonts(App& app) {
    const auto old_font = app.font;
    app.font = CreateFontW(-MulDiv(9, static_cast<int>(app.dpi), 72), 0, 0, 0, FW_NORMAL,
                          FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (HWND child = GetWindow(app.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
    }
    if (old_font) { DeleteObject(old_font); }
}

void layout(App& app) {
    RECT client{};
    GetClientRect(app.window, &client);
    const int width = MulDiv(client.right, 96, static_cast<int>(app.dpi));
    const int height = MulDiv(client.bottom, 96, static_cast<int>(app.dpi));
    const int margin = 16;
    const int content = width - margin * 2;
    auto move = [&](HWND control, int x, int y, int w, int h) {
        MoveWindow(control, scaled(app, x), scaled(app, y), scaled(app, std::max(w, 1)),
                   scaled(app, std::max(h, 1)), TRUE);
    };
    move(app.device_label, margin, 22, 40, 20);
    move(app.device, 62, 16, width - 78, 200);
    move(app.port_label, margin, 58, 40, 20);
    move(app.port, 62, 52, width - 162, 200);
    move(app.refresh, width - 88, 52, 72, 26);
    const int button_width = (content - 12) / 2;
    move(app.connect, margin, 96, button_width, 32);
    move(app.clear, margin + button_width + 12, 96, content - button_width - 12, 32);
    move(app.status, margin, 146, content, 22);
    move(app.progress, margin, 174, content, 6);
    move(app.help, margin, height - 44, 88, 28);
    move(app.view_log, margin + 100, height - 44, 88, 28);
}

HWND control(App& app, const wchar_t* kind, const wchar_t* text, DWORD style, int id = 0,
             DWORD extended = 0) {
    return CreateWindowExW(extended, kind, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
                           app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

bool create_controls(App& app) {
    app.device_label = control(app, L"STATIC", L"MCU", SS_LEFT);
    app.device = control(app, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id_device);
    SendMessageW(app.device, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"R5F21548G"));
    SendMessageW(app.device, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"R5F21347G"));
    app.port_label = control(app, L"STATIC", L"COM", SS_LEFT);
    app.port = control(app, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id_port);
    app.refresh = control(app, L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, id_refresh);
    app.connect = control(app, L"BUTTON", L"Connect", BS_PUSHBUTTON | WS_TABSTOP, id_connect);
    app.clear = control(app, L"BUTTON", L"Clear ID", BS_PUSHBUTTON | WS_TABSTOP, id_clear);
    app.help = control(app, L"BUTTON", L"วิธีใช้", BS_PUSHBUTTON | WS_TABSTOP, id_help);
    app.view_log = control(app, L"BUTTON", L"บันทึก", BS_PUSHBUTTON | WS_TABSTOP, id_log);
    app.status = control(app, L"STATIC", L"ยังไม่เชื่อมต่อ", SS_LEFT | SS_ENDELLIPSIS);
    app.progress = control(app, PROGRESS_CLASSW, L"", PBS_MARQUEE);
    const HWND required[] = {app.device_label, app.device, app.port_label, app.port, app.refresh,
        app.connect, app.clear, app.help, app.view_log, app.status, app.progress};
    return std::all_of(std::begin(required), std::end(required), [](HWND handle) { return handle != nullptr; });
}

void begin_operation(App& app, Operation operation) {
    if (app.busy) { return; }
    const auto port = window_text(app.port);
    const auto device = selected_device(app);
    if (operation == Operation::Connect && (app.connected || !device || !r8c::valid_port(port))) { return; }
    if (operation != Operation::Connect && !app.connected) { return; }
    if (operation == Operation::Clear && !device) { return; }
    bool acknowledged = false;
    if (operation == Operation::Clear) {
        const auto& profile = r8c::device_profile(*device);
        const std::wstring name(profile.name, profile.name + std::char_traits<char>::length(profile.name));
        const std::wstring warning = L"ยืนยันลบ Program ROM และ Data Flash ทั้งหมดของ " + name +
            L"?\n\nจำนวน " + std::to_wstring(profile.end_address - profile.first_address) +
            L" ไบต์ การลบนี้ไม่สามารถย้อนกลับได้";
        const int answer = MessageBoxW(app.window, warning.c_str(),
            app_title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (answer != IDYES) { return; }
        acknowledged = true;
    }
    if (app.worker.joinable()) { app.worker.join(); }
    app.busy = true;
    app.operation = operation;
    const bool previously_connected = app.connected;
    const wchar_t* state = operation == Operation::Connect ? L"กำลังเชื่อมต่อ..."
        : operation == Operation::Clear ? L"กำลังล้างข้อมูล..."
        : L"กำลังตัดการเชื่อมต่อ...";
    SetWindowTextW(app.status, state);
    update_controls(app);
    if (operation == Operation::Clear && device) {
        const auto& profile = r8c::device_profile(*device);
        const std::uint32_t total = profile.end_address - profile.first_address;
        {
            std::lock_guard lock(app.mutex);
            app.prog_done = 0;
            app.prog_total = total;
        }
        app.prog_start_ms = GetTickCount64();
        SendMessageW(app.progress, PBM_SETRANGE32, 0, static_cast<LPARAM>(total));
        SendMessageW(app.progress, PBM_SETPOS, 0, 0);
    }
    try {
        app.worker = std::thread([&app, operation, port, device, acknowledged, previously_connected] {
            r8c::JobResult outcome;
            try {
                switch (operation) {
                case Operation::Connect: outcome = app.session.connect(port, *device, {}); break;
                case Operation::Clear:
                    outcome = app.session.clear_id(acknowledged, {}, [&app](std::uint32_t done, std::uint32_t total) {
                        std::lock_guard lock(app.mutex);
                        app.prog_done = done;
                        app.prog_total = total;
                    });
                    break;
                case Operation::Disconnect: outcome = app.session.disconnect(); break;
                }
            } catch (...) {
                outcome.connected = previously_connected;
                outcome.message = L"เกิดข้อผิดพลาดภายใน ไม่สามารถยืนยันผลได้ โปรดตรวจบันทึกและสถานะอุปกรณ์";
            }
            std::lock_guard lock(app.mutex);
            app.result = std::move(outcome);
        });
    } catch (...) {
        app.busy = false;
        app.close_after_disconnect = false;
        SetWindowTextW(app.status, L"ไม่สามารถเริ่มงานได้");
        update_controls(app);
    }
}

void update_progress(App& app) {
    if (!app.busy || app.operation != Operation::Clear) { return; }
    std::uint32_t done = 0;
    std::uint32_t total = 0;
    {
        std::lock_guard lock(app.mutex);
        done = app.prog_done;
        total = app.prog_total;
    }
    if (total == 0) { return; }
    if (done > total) { done = total; }
    SendMessageW(app.progress, PBM_SETPOS, static_cast<WPARAM>(done), 0);
    const double pct = 100.0 * static_cast<double>(done) / static_cast<double>(total);
    std::wstring text = L"ล้าง/ตรวจ " + fmt1(pct) + L"% • " +
        std::to_wstring(done / 1024) + L"/" + std::to_wstring(total / 1024) + L"KB";
    const std::uint64_t elapsed_ms = GetTickCount64() - app.prog_start_ms;
    if (done > 0 && elapsed_ms > 800) {
        const double rate = static_cast<double>(done) / (static_cast<double>(elapsed_ms) / 1000.0);
        if (rate > 1.0) {
            text += L" • " + fmt1(rate / 1024.0) + L"KB/s • เหลือ ~" + fmt_eta(static_cast<double>(total - done) / rate);
        }
    }
    SetWindowTextW(app.status, text.c_str());
}

void poll_worker(App& app) {
    std::optional<r8c::JobResult> result;
    {
        std::lock_guard lock(app.mutex);
        result.swap(app.result);
    }
    if (!result) { return; }
    if (app.worker.joinable()) { app.worker.join(); }
    app.busy = false;
    app.connected = result->connected;
    if (!result->log_directory.empty()) { app.log_directory = result->log_directory; }
    std::wstring status = L"ยังยืนยันผลไม่ได้";
    switch (app.operation) {
    case Operation::Connect:
        status = result->success && result->connected ? L"เชื่อมต่อแล้ว" : L"เชื่อมต่อไม่สำเร็จ";
        break;
    case Operation::Clear:
        if (result->success && result->erase_verified) {
            status = L"ล้างและตรวจสอบสำเร็จ";
            std::uint32_t total = 0;
            { std::lock_guard lock(app.mutex); total = app.prog_total; }
            const std::uint64_t elapsed_ms = GetTickCount64() - app.prog_start_ms;
            if (total > 0 && elapsed_ms > 0) {
                const double secs = static_cast<double>(elapsed_ms) / 1000.0;
                const double kbs = static_cast<double>(total) / 1024.0 / secs;
                status += L" • " + std::to_wstring(total / 1024) + L"KB • " + fmt1(secs) + L"วิ • " + fmt1(kbs) + L"KB/s";
            }
            SendMessageW(app.progress, PBM_SETPOS, static_cast<WPARAM>(total), 0);
        } else {
            status = L"ยังยืนยันผลไม่ได้";
        }
        break;
    case Operation::Disconnect:
        status = !result->connected ? L"ตัดการเชื่อมต่อแล้ว" : L"ยังเชื่อมต่ออยู่";
        break;
    }
    SetWindowTextW(app.status, status.c_str());
    update_controls(app);
    if (app.close_after_disconnect) {
        app.close_after_disconnect = false;
        if (!app.connected) { DestroyWindow(app.window); }
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<App*>(create->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) { return DefWindowProcW(window, message, wparam, lparam); }
    switch (message) {
    case WM_CREATE:
        app->dpi = GetDpiForWindow(window);
        if (!create_controls(*app)) { return -1; }
        create_fonts(*app);
        refresh_ports(*app);
        layout(*app);
        if (SetTimer(window, poll_timer, 100, nullptr) == 0) { return -1; }
        return 0;
    case WM_SIZE:
        if (app->port) { layout(*app); }
        return 0;
    case WM_DPICHANGED: {
        app->dpi = HIWORD(wparam);
        create_fonts(*app);
        const auto* rectangle = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, rectangle->left, rectangle->top,
                     rectangle->right - rectangle->left, rectangle->bottom - rectangle->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout(*app);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        RECT minimum{0, 0, scaled(*app, 380), scaled(*app, 250)};
        AdjustWindowRectExForDpi(&minimum, WS_OVERLAPPEDWINDOW, FALSE, 0, app->dpi);
        limits->ptMinTrackSize = {minimum.right - minimum.left, minimum.bottom - minimum.top};
        return 0;
    }
    case WM_TIMER:
        if (wparam == poll_timer) { update_progress(*app); poll_worker(*app); }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case id_device:
        case id_port:
            if (HIWORD(wparam) == CBN_SELCHANGE) { update_controls(*app); }
            break;
        case id_refresh:
            if (!app->busy && !app->connected) { refresh_ports(*app); }
            break;
        case id_help:
            MessageBoxW(window,
                L"รุ่นที่รองรับ: R5F21548G และ R5F21347G\n\n"
                L"1. เลือก MCU ให้ตรงตัวอักษรบนชิป แล้วต่อวงจรโปรแกรมเมอร์ที่ตรวจสอบแล้ว\n"
                L"2. ปิดโปรแกรมอื่นที่ใช้ COM และตั้ง MODE / RESET เข้า serial boot ด้วยตนเอง\n"
                L"3. เลือก COM แล้วกด Connect โดยไม่ต่อใช้งาน DTR / RTS\n"
                L"4. กด Clear ID และยืนยันเมื่อต้องการลบ Program ROM / Data Flash ทั้งหมด\n\n"
                L"Forced Erase ใช้ได้เฉพาะเมื่อ MCU อนุญาต ไม่ข้าม Code Protect\n"
                L"ห้ามถอดสายหรือตัดไฟระหว่างงาน หลังล้างต้องเขียนเฟิร์มแวร์ที่ถูกต้องกลับ",
                L"วิธีใช้", MB_OK | MB_ICONINFORMATION);
            break;
        case id_connect: begin_operation(*app, app->connected ? Operation::Disconnect : Operation::Connect); break;
        case id_clear: begin_operation(*app, Operation::Clear); break;
        case id_log: {
            const auto path = (std::filesystem::path(app->log_directory) / L"session.log").wstring();
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", path.c_str(),
                                                        nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
                MessageBoxW(window, L"ไม่สามารถเปิดบันทึกได้", app_title, MB_OK | MB_ICONERROR);
            }
            break;
        }
        case IDCANCEL: SendMessageW(window, WM_CLOSE, 0, 0); break;
        }
        return 0;
    case WM_CLOSE:
        if (app->busy) {
            MessageBoxW(window, L"ยังมีงานกำลังทำงาน โปรดรอผลก่อนปิดโปรแกรม\nห้ามถอดสายหรือตัดไฟระหว่างการล้าง",
                        app_title, MB_OK | MB_ICONINFORMATION);
        } else if (app->connected) {
            app->close_after_disconnect = true;
            begin_operation(*app, Operation::Disconnect);
        } else {
            DestroyWindow(window);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, poll_timer);
        if (app->worker.joinable()) { app->worker.join(); }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    if (!InitCommonControlsEx(&controls)) { return 1; }
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = window_proc;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    type.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    type.lpszClassName = L"AlexDevTH.R8CIdCheckClear.Window";
    if (!RegisterClassExW(&type)) { return 1; }
    App app;
    const auto window = CreateWindowExW(0, type.lpszClassName, app_title, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 436, 299, nullptr, nullptr, instance, &app);
    if (!window) {
        MessageBoxW(nullptr, L"ไม่สามารถสร้างหน้าต่างโปรแกรมได้", app_title, MB_OK | MB_ICONERROR);
        if (app.font) { DeleteObject(app.font); }
        return 1;
    }
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT initial{0, 0, scaled(app, 470), scaled(app, 260)};
    AdjustWindowRectExForDpi(&initial, WS_OVERLAPPEDWINDOW, FALSE, 0, app.dpi);
    const int width = std::min(static_cast<int>(initial.right - initial.left), static_cast<int>(work.right - work.left));
    const int height = std::min(static_cast<int>(initial.bottom - initial.top), static_cast<int>(work.bottom - work.top));
    SetWindowPos(window, nullptr, work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message{};
    BOOL result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (app.font) { DeleteObject(app.font); }
    return result < 0 ? 1 : static_cast<int>(message.wParam);
}
