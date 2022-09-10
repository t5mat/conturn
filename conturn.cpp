#include <cwchar>
#include <cstdio>
#include <optional>
#include <algorithm>
#include <windows.h>
#include <pathcch.h>
#include <shlobj.h>

namespace common {

template<typename T>
constexpr std::enable_if_t<std::is_enum<T>::value, std::underlying_type_t<T>> operator*(T v)
{
    return std::underlying_type_t<T>(v);
}

namespace win32 {

namespace {

using ZwSetTimerResolution_t = NTSTATUS (WINAPI *)(IN ULONG RequestedResolution, IN BOOLEAN Set, OUT PULONG ActualResolution);
auto ZwSetTimerResolution = reinterpret_cast<ZwSetTimerResolution_t>(GetProcAddress(LoadLibraryW(L"ntdll.dll"), "ZwSetTimerResolution"));

using NtDelayExecution_t = NTSTATUS (WINAPI *)(IN BOOL Alertable, IN PLARGE_INTEGER DelayInterval);
auto NtDelayExecution = reinterpret_cast<NtDelayExecution_t>(GetProcAddress(LoadLibraryW(L"ntdll.dll"), "NtDelayExecution"));

long long performance_counter_frequency()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

}

const auto SHELL_TASKBAR_CREATED_MSG = RegisterWindowMessageW(L"TaskbarCreated");

const auto PERFORMANCE_COUNTER_FREQUENCY = performance_counter_frequency();

long long performance_counter()
{
    LARGE_INTEGER i;
    QueryPerformanceCounter(&i);
    return i.QuadPart;
}

void set_timer_resolution(unsigned long hns)
{
    ULONG actual;
    ZwSetTimerResolution(hns, true, &actual);
}

void delay_execution_by(long long hns)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -1 * hns;
    NtDelayExecution(false, &interval);
}

HWND create_window(const wchar_t *class_name, const wchar_t *window_name, WNDPROC proc)
{
    auto instance = GetModuleHandle(nullptr);

    WNDCLASSEXW cls = {};
    cls.cbSize = sizeof(WNDCLASSEX);
    cls.lpfnWndProc = proc;
    cls.hInstance = instance;
    cls.lpszClassName = class_name;
    RegisterClassExW(&cls);

    return CreateWindowExW(0, class_name, window_name, 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
}

void move_mouse_by(int x, int y)
{
    INPUT input;
    input.type = INPUT_MOUSE;
    input.mi.dx = x;
    input.mi.dy = y;
    input.mi.mouseData = 0;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.time = 0;
    input.mi.dwExtraInfo = 0;
    SendInput(1, &input, sizeof(input));
}

void open_folder_and_select(const wchar_t *path)
{
    PIDLIST_ABSOLUTE item;
    SFGAOF sfgaof;
    if (S_OK != SHParseDisplayName(path, nullptr, &item, 0, &sfgaof)) {
        return;
    }
    SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    CoTaskMemFree(item);
}

}

}

using namespace common;

inline constexpr size_t CFG_MAX_COUNT = 8192;
inline constexpr size_t CON_LINE_MAX_COUNT = 8192;
inline constexpr size_t CON_VAR_MAX_COUNT = 256;

struct VersionInfo
{
    wchar_t name[128];
    wchar_t title[128];
    wchar_t version[128];
    wchar_t copyright[128];

    static VersionInfo load(const wchar_t *path)
    {
        VersionInfo info;

        DWORD handle;
        auto size = GetFileVersionInfoSizeW(path, &handle);

        auto buffer = std::malloc(size);

        GetFileVersionInfoW(path, handle, size, buffer);

        UINT count;
        wchar_t *s;

        VerQueryValueW(buffer, L"\\StringFileInfo\\040904E4\\InternalName", reinterpret_cast<void **>(&s), &count);
        std::swprintf(info.name, std::size(info.name), L"%s", s);

        VerQueryValueW(buffer, L"\\StringFileInfo\\040904E4\\ProductName", reinterpret_cast<void **>(&s), &count);
        std::swprintf(info.title, std::size(info.title), L"%s", s);

        VerQueryValueW(buffer, L"\\StringFileInfo\\040904E4\\ProductVersion", reinterpret_cast<void **>(&s), &count);
        std::swprintf(info.version, std::size(info.version), L"%s", s);

        VerQueryValueW(buffer, L"\\StringFileInfo\\040904E4\\LegalCopyright", reinterpret_cast<void **>(&s), &count);
        std::swprintf(info.copyright, std::size(info.copyright), L"%s", s);

        std::free(buffer);

        return info;
    }
};

struct ConLogPipe
{
    static std::optional<ConLogPipe> try_create(const wchar_t *path)
    {
        // https://stackoverflow.com/a/38413449

        SECURITY_DESCRIPTOR sd;
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, true, nullptr, false);

        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = false;

        auto pipe = CreateNamedPipeW(path, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, sizeof(buffer), sizeof(buffer), 0, &sa);
        if (pipe == INVALID_HANDLE_VALUE) {
            return std::nullopt;
        }

        auto event = CreateEventW(nullptr, true, true, nullptr);
        return ConLogPipe(pipe, event);
    }

    auto event()
    {
        return overlapped.hEvent;
    }

    auto connected()
    {
        return (state == State::READING);
    }

    auto client_pid()
    {
        return client_pid_;
    }

    bool get_next_line(char (&line)[CON_LINE_MAX_COUNT])
    {
    loop:
        if (pending) {
            DWORD transferred;
            auto success = GetOverlappedResult(pipe, &overlapped, &transferred, false);

            if (success) {
                switch (state) {
                case State::CONNECTING:
                    state = State::READING;
                    pending = false;
                    GetNamedPipeClientProcessId(pipe, &client_pid_);
                    goto loop;
                case State::READING:
                    buffer_count += transferred;
                    pending = false;
                    goto loop;
                }
            }

            switch (GetLastError()) {
            case ERROR_IO_INCOMPLETE:
                return false;
            }

            switch (state) {
            case State::CONNECTING:
                pending = false;
                goto loop;
            case State::READING:
                state = State::DISCONNECTING;
                pending = false;
                goto loop;
            }
        }

        switch (state) {
        case State::CONNECTING:
            ConnectNamedPipe(pipe, &overlapped);

            switch (GetLastError()) {
            case ERROR_IO_PENDING:
                pending = true;
                return false;
            case ERROR_PIPE_CONNECTED:
                state = State::READING;
                goto loop;
            default:
                return false;
            }
        case State::READING:
            {
                for (size_t i = 0; i < buffer_count; ++i) {
                    if (buffer[i] == '\n') {
                        std::strncpy(line, buffer, i - 1);
                        line[i - 1] = '\0';

                        std::memmove(buffer, buffer + (i + 1), (std::size(buffer) - (i + 1)) * sizeof(*buffer));
                        buffer_count -= i + 1;

                        return true;
                    }
                }

                DWORD read;
                auto success = ReadFile(pipe, buffer + buffer_count, (std::size(buffer) - buffer_count - 1) * sizeof(*buffer), &read, &overlapped);

                if (success) {
                    buffer_count += read;
                    goto loop;
                }

                switch (GetLastError()) {
                case ERROR_IO_PENDING:
                    pending = true;
                    return false;
                default:
                    state = State::DISCONNECTING;
                    goto loop;
                }
            }
        case State::DISCONNECTING:
            DisconnectNamedPipe(pipe);
            state = State::CONNECTING;
            goto loop;
        }
    }

private:
    explicit ConLogPipe(HANDLE pipe_, HANDLE event)
    {
        pending = false;
        state = State::CONNECTING;
        pipe = pipe_;

        overlapped = {};
        overlapped.hEvent = event;

        buffer_count = 0;
    }

    enum class State
    {
        CONNECTING,
        READING,
        DISCONNECTING
    };

    bool pending;
    State state;
    HANDLE pipe;
    OVERLAPPED overlapped;
    DWORD client_pid_;
    char buffer[CON_LINE_MAX_COUNT];
    size_t buffer_count;
};

struct MouseMoveCalculator
{
    long long update(bool reset, bool in_left, bool in_right, bool in_speed, double freq, double yawspeed, double anglespeedkey, double sensitivity, double yaw)
    {
        if (reset) {
            last_in_left = false;
            last_in_right = false;
        }

        auto time = win32::performance_counter();

        if ((last_in_left ^ in_left) || (last_in_right ^ in_right)) {
            last_time = time;
            remaining = 0.0;
        }

        last_in_left = in_left;
        last_in_right = in_right;

        if (!(in_left ^ in_right) || (time - last_time < win32::PERFORMANCE_COUNTER_FREQUENCY * freq)) {
            return 0;
        }

        remaining +=
            (
                (int(in_left) * -1 + int(in_right)) *
                (yawspeed / (sensitivity * yaw)) *
                (in_speed ? anglespeedkey : 1.0) *
                (time - last_time)
            ) / win32::PERFORMANCE_COUNTER_FREQUENCY;

        auto amount = static_cast<long long>(remaining);
        remaining -= amount;
        last_time = time;
        return amount;
    }

private:
    long long last_time;
    double remaining;
    bool last_in_left;
    bool last_in_right;
};

struct ForegroundMonitor
{
    ForegroundMonitor(const wchar_t *target_path_)
    {
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, proc, 0, 0, WINEVENT_OUTOFCONTEXT);
        GetWindowThreadProcessId(GetForegroundWindow(), &pid_);
    }

    auto pid()
    {
        return pid_;
    }

private:
    static void proc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD idEventThread, DWORD dwmsEventTime)
    {
        GetWindowThreadProcessId(hwnd, &pid_);
    }

    static inline DWORD pid_;
};

struct CursorMonitor
{
    CursorMonitor()
    {
        SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, proc, 0, 0, WINEVENT_OUTOFCONTEXT);

        CURSORINFO info = {};
        info.cbSize = sizeof(info);
        GetCursorInfo(&info);
        cursor_ = info.flags & CURSOR_SHOWING;
    }

    auto cursor()
    {
        return cursor_;
    }

private:
    static void proc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD idEventThread, DWORD dwmsEventTime)
    {
        if (idObject == OBJID_CURSOR) {
            cursor_ = (event == EVENT_OBJECT_SHOW);
        }
    }

    static inline bool cursor_;
};

struct CtrlSignalHandler
{
    CtrlSignalHandler(HWND hwnd_)
    {
        hwnd = hwnd_;
        event = CreateEventW(nullptr, false, false, nullptr);

        SetConsoleCtrlHandler(proc, true);
    }

    void done()
    {
        SetEvent(event);
    }

private:
    static BOOL proc(DWORD dwCtrlType)
    {
        switch (dwCtrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            PostMessageW(hwnd, WM_QUIT, 0, 0);
            WaitForSingleObject(event, INFINITE);
            return true;
        }
        return false;
    }

    static inline HWND hwnd;
    static inline HANDLE event;
};

struct TrayIcon
{
    using CreateMenu_t = HMENU (*)();

    explicit TrayIcon(HWND hwnd, const wchar_t *tip, CreateMenu_t create_menu_)
    {
        data = {};
        data.uVersion = NOTIFYICON_VERSION_4;
        data.cbSize = sizeof(data);
        data.hWnd = hwnd;
        data.uID = 1;
        data.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_MESSAGE;
        data.uCallbackMessage = WINDOW_MSG;
        data.hIcon = static_cast<HICON>(::LoadImageW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

        create_menu = create_menu_;

        Shell_NotifyIconW(NIM_ADD, &data);
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }

    ~TrayIcon()
    {
        Shell_NotifyIconW(NIM_DELETE, &data);
    }

    void set_tip(const wchar_t *tip)
    {
        std::swprintf(data.szTip, std::size(data.szTip), L"%s", tip);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    bool handle_msg(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        if (uMsg == win32::SHELL_TASKBAR_CREATED_MSG) {
            Shell_NotifyIconW(NIM_ADD, &data);
            Shell_NotifyIconW(NIM_SETVERSION, &data);
            return true;
        }

        switch (uMsg) {
        case WINDOW_MSG:
            switch (LOWORD(lParam)) {
            case WM_CONTEXTMENU:
                {
                    auto menu = create_menu();

                    POINT point;
                    GetCursorPos(&point);

                    SetForegroundWindow(hwnd);
                    TrackPopupMenuEx(menu, TPM_LEFTBUTTON | TPM_LEFTALIGN | TPM_BOTTOMALIGN, point.x, point.y, hwnd, nullptr);
                    PostMessageW(hwnd, WM_NULL, 0, 0);

                    DestroyMenu(menu);
                }
                break;
            }
        default:
            return false;
        }
    }

private:
    static constexpr auto WINDOW_MSG = WM_USER + 0;

    NOTIFYICONDATAW data;

    CreateMenu_t create_menu;
};

struct ConVar
{
    char name[CON_VAR_MAX_COUNT];

    char value[CON_VAR_MAX_COUNT];
    union {
        double double_value;
        bool bool_value;
    };

    ConVar(const char *name_)
    {
        std::snprintf(name, std::size(name), "%s", name_);
        std::snprintf(pattern, std::size(pattern), R"("%s" = ")", name_);
    }

    void parse_double()
    {
        double_value = std::atof(value);
    }

    void parse_bool()
    {
        bool_value = std::atof(value);
    }

    bool parse_con_cvar_line(const char *line)
    {
        auto start = line;
        for (; pattern[start - line] != '\0'; ++start) {
            if (*start != pattern[start - line]) {
                return false;
            }
        }

        for (size_t i = 0; start[i] != '\0'; ++i) {
            if (start[i] == '"') {
                auto count = std::min(std::size(value), i + 1);
                std::strncpy(value, start, count - 1);
                value[count - 1] = L'\0';
                return true;
            }
        }

        return false;
    }

private:
    char pattern[CON_VAR_MAX_COUNT];
};

enum class Command
{
    ABOUT,
    OPEN_FOLDER,
    OPEN_GAME_FOLDER,
    EXIT
};

struct App
{
    static void run()
    {
        GetModuleFileNameW(nullptr, image_path, std::size(image_path));

        version_info = VersionInfo::load(image_path);

        std::swprintf(pipe_path, std::size(pipe_path), LR"(\\.\pipe\%s-log)", version_info.name);
        std::swprintf(cfg_filename, std::size(cfg_filename), LR"(%s.cfg)", version_info.name);
        std::swprintf(con_log_filename, std::size(con_log_filename), LR"(%s.log)", version_info.name);

        con_log_pipe = ConLogPipe::try_create(pipe_path);
        if (!con_log_pipe) {
            wchar_t text[128];
            std::swprintf(text, std::size(text), L"%s is already running!", version_info.title);
            MessageBoxW(nullptr, text, version_info.title, MB_OK | MB_ICONWARNING);
            return;
        }

        std::wcscpy(ini_path, image_path);
        PathCchRenameExtension(ini_path, std::size(ini_path), LR"(ini)");

        GetPrivateProfileStringW(version_info.name, L"GamePath", L"", game_path, std::size(game_path), ini_path);
        if (INVALID_FILE_ATTRIBUTES == GetFileAttributesW(game_path)) {
            if (!show_game_path_dialog(game_path)) {
                return;
            }
            WritePrivateProfileStringW(version_info.name, L"GamePath", game_path, ini_path);
        }

        std::wcscpy(cfg_path, game_path);
        PathCchRemoveFileSpec(cfg_path, std::size(cfg_path));
        PathCchAppend(cfg_path, std::size(cfg_path), LR"(csgo\cfg)");
        PathCchAppend(cfg_path, std::size(cfg_path), cfg_filename);

        std::wcscpy(con_log_path, game_path);
        PathCchRemoveFileSpec(con_log_path, std::size(con_log_path));
        PathCchAppend(con_log_path, std::size(con_log_path),LR"(csgo)");
        PathCchAppend(con_log_path, std::size(con_log_path), con_log_filename);

        create_con_vars();
        init_con_vars();
        ini_read_con_vars();

        delete_cfg_file();
        delete_con_log_file();
        create_con_log_file();
        create_cfg_file(true);

        MouseMoveCalculator mouse_move_calculator;
        ForegroundMonitor foreground_monitor(game_path);
        CursorMonitor cursor_monitor;
        HWND hwnd = win32::create_window(version_info.name, version_info.name, window_proc);
        CtrlSignalHandler ctrl_signal_handler(hwnd);
        tray_icon.emplace(hwnd, version_info.title, create_tray_menu);

        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        win32::set_timer_resolution(1);

        HANDLE game_process = nullptr;
        std::optional<bool> last_connected;
        bool active = false;

        do {
            bool handle_window = active;
            bool handle_con_log_pipe = active;
            bool handle_game_process = false;

            if (!active) {
                if (!game_process) {
                    HANDLE handles[] = {con_log_pipe->event()};
                    auto result = MsgWaitForMultipleObjects(std::size(handles), handles, false, INFINITE, QS_ALLINPUT);
                    switch (result) {
                    case WAIT_OBJECT_0 + 0:
                        handle_con_log_pipe = true;
                        break;
                    case WAIT_OBJECT_0 + 1:
                        handle_window = true;
                        break;
                    }
                } else {
                    HANDLE handles[] = {con_log_pipe->event(), game_process};
                    auto result = MsgWaitForMultipleObjects(std::size(handles), handles, false, INFINITE, QS_ALLINPUT);
                    switch (result) {
                    case WAIT_OBJECT_0 + 0:
                        handle_con_log_pipe = true;
                        break;
                    case WAIT_OBJECT_0 + 1:
                        handle_game_process = true;
                        break;
                    case WAIT_OBJECT_0 + 2:
                        handle_window = true;
                        break;
                    }
                }
            }

            if (handle_window) {
            peek:
                MSG msg;
                if (PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        break;
                    }
                    DispatchMessage(&msg);
                    goto peek;
                }
            }

            if (handle_con_log_pipe) {
                char line[CON_LINE_MAX_COUNT];
                while (con_log_pipe->get_next_line(line)) {
                    handle_con_line(line);
                }

                bool connected = con_log_pipe->connected();
                if (!last_connected || *last_connected != connected) {
                    if (connected && !game_process) {
                        game_process = OpenProcess(SYNCHRONIZE, false, con_log_pipe->client_pid());
                    }

                    wchar_t buffer[128];
                    if (connected) {
                        std::swprintf(buffer, std::size(buffer), L"%s (attached: PID %d)", version_info.title, con_log_pipe->client_pid());
                    } else {
                        std::swprintf(buffer, std::size(buffer), L"%s (not attached)", version_info.title);
                    }
                    tray_icon->set_tip(buffer);
                }
                last_connected = connected;
            }

            if (handle_game_process) {
                CloseHandle(game_process);
                game_process = nullptr;

                create_cfg_file(true);
                ini_write_con_vars();
            }

            auto last_active = active;
            active = !cursor_monitor.cursor() && con_log_pipe->connected() && foreground_monitor.pid() == con_log_pipe->client_pid();
            active = active && freq->double_value >= 0.0 && (sleep->double_value == -1.0 || (sleep->double_value >= 0.0 && sleep->double_value < 0.5)) && sensitivity->double_value != 0.0 && yaw->double_value != 0.0;
            if (!active) {
                continue;
            }

            auto amount = mouse_move_calculator.update(!last_active, in_left->bool_value, in_right->bool_value, in_speed->bool_value, freq->double_value, yawspeed->double_value, anglespeedkey->double_value, sensitivity->double_value, yaw->double_value);
            if (amount != 0) {
                win32::move_mouse_by(amount, 0);
            }

            if (sleep->double_value != -1.0) {
                win32::delay_execution_by(sleep->double_value * 10000000);
            }
        } while (true);

        tray_icon.reset();

        delete_cfg_file();
        delete_con_log_file();

        ini_write_con_vars();

        ctrl_signal_handler.done();
    }

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        switch (uMsg) {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_COMMAND:
            switch (static_cast<Command>(LOWORD(wParam))) {
            case Command::ABOUT:
                ShellExecuteW(nullptr, nullptr, version_info.copyright, nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case Command::OPEN_FOLDER:
                win32::open_folder_and_select(image_path);
                break;
            case Command::OPEN_GAME_FOLDER:
                win32::open_folder_and_select(game_path);
                break;
            case Command::EXIT:
                PostMessageW(hwnd, WM_QUIT, 0, 0);
                break;
            }
        default:
            if (tray_icon->handle_msg(hwnd, uMsg, wParam, lParam)) {
                return 0;
            }
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
        return 0;
    }

    static HMENU create_tray_menu()
    {
        wchar_t buffer[128];

        auto menu = CreatePopupMenu();

        std::swprintf(buffer, std::size(buffer), L"%s %s", version_info.title, version_info.version);
        AppendMenuW(menu, MF_STRING, *Command::ABOUT, buffer);

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        if (con_log_pipe->connected()) {
            std::swprintf(buffer, std::size(buffer), L"(attached: PID %d)", con_log_pipe->client_pid());
        } else {
            std::swprintf(buffer, std::size(buffer), L"(not attached)");
        }
        AppendMenuW(menu, MF_GRAYED, 0, buffer);

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        AppendMenuW(menu, MF_STRING, *Command::OPEN_GAME_FOLDER, L"Open game folder...");

        std::swprintf(buffer, std::size(buffer), L"Open %s folder...", version_info.title);
        AppendMenuW(menu, MF_STRING, *Command::OPEN_FOLDER, buffer);

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        AppendMenuW(menu, MF_STRING, *Command::EXIT, L"Exit");

        return menu;
    }

    static bool show_game_path_dialog(wchar_t (&path)[PATHCCH_MAX_CCH])
    {
        OPENFILENAMEW info;
        info.lStructSize = sizeof(OPENFILENAMEW);
        info.hwndOwner = nullptr;
        info.hInstance = nullptr;
        info.lpstrFilter = L"csgo.exe\0csgo.exe\0";
        info.lpstrCustomFilter = nullptr;
        info.nFilterIndex = 0;
        info.lpstrFile = path;
        info.nMaxFile = std::size(path);
        info.lpstrFileTitle = nullptr;
        info.lpstrInitialDir = nullptr;
        info.lpstrTitle = L"Select your csgo.exe";
        info.Flags = OFN_DONTADDTORECENT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        info.nFileOffset = 0;
        info.nFileExtension = 0;
        info.lpstrDefExt = nullptr;
        info.FlagsEx = 0;
        return GetOpenFileNameW(&info);
    }

    static void create_con_vars()
    {
        std::snprintf(off_alias_name, std::size(off_alias_name), "%S_off", version_info.name);
        std::snprintf(left_alias_name, std::size(left_alias_name), "_left");
        std::snprintf(right_alias_name, std::size(right_alias_name), "_right");
        std::snprintf(speed_alias_name, std::size(speed_alias_name), "_speed");

        char name[CON_VAR_MAX_COUNT];

        std::snprintf(name, std::size(name), "%S_version", version_info.name);
        version.emplace(name);

        std::snprintf(name, std::size(name), "%S_url", version_info.name);
        url.emplace(name);

        std::snprintf(name, std::size(name), "%S_freq", version_info.name);
        freq.emplace(name);

        std::snprintf(name, std::size(name), "%S_sleep", version_info.name);
        sleep.emplace(name);

        yawspeed.emplace("_cl_yawspeed");
        anglespeedkey.emplace("_cl_anglespeedkey");
        sensitivity.emplace("sensitivity");
        yaw.emplace("m_yaw");
        in_left.emplace("_in_left");
        in_right.emplace("_in_right");
        in_speed.emplace("_in_speed");
    }

    static void init_con_vars()
    {
        std::snprintf(version->value, std::size(version->value), "%S %S", version_info.title, version_info.version);
        std::snprintf(url->value, std::size(url->value), "%S", version_info.copyright);

        std::strcpy(sensitivity->value, "");
        sensitivity->parse_double();

        std::strcpy(yaw->value, "");
        yaw->parse_double();

        std::strcpy(in_left->value, "0");
        in_left->parse_bool();

        std::strcpy(in_right->value, "0");
        in_right->parse_bool();

        std::strcpy(in_speed->value, "0");
        in_speed->parse_bool();
    }

    static void ini_read_con_vars()
    {
        wchar_t value[CON_VAR_MAX_COUNT];

        GetPrivateProfileStringW(version_info.name, L"Freq", L"0.001", value, std::size(value), ini_path);
        std::snprintf(freq->value, std::size(freq->value), "%S", value);
        freq->parse_double();

        GetPrivateProfileStringW(version_info.name, L"Sleep", L"0.0000005", value, std::size(value), ini_path);
        std::snprintf(sleep->value, std::size(sleep->value), "%S", value);
        sleep->parse_double();

        GetPrivateProfileStringW(version_info.name, L"YawSpeed", L"90.0", value, std::size(value), ini_path);
        std::snprintf(yawspeed->value, std::size(yawspeed->value), "%S", value);
        yawspeed->parse_double();

        GetPrivateProfileStringW(version_info.name, L"AngleSpeedKey", L"0.33", value, std::size(value), ini_path);
        std::snprintf(anglespeedkey->value, std::size(anglespeedkey->value), "%S", value);
        anglespeedkey->parse_double();
    }

    static void ini_write_con_vars()
    {
        wchar_t value[CON_VAR_MAX_COUNT];

        std::swprintf(value, std::size(value), L"%S", freq->value);
        WritePrivateProfileStringW(version_info.name, L"Freq", value, ini_path);

        std::swprintf(value, std::size(value), L"%S", sleep->value);
        WritePrivateProfileStringW(version_info.name, L"Sleep", value, ini_path);

        std::swprintf(value, std::size(value), L"%S", yawspeed->value);
        WritePrivateProfileStringW(version_info.name, L"YawSpeed", value, ini_path);

        std::swprintf(value, std::size(value), L"%S", anglespeedkey->value);
        WritePrivateProfileStringW(version_info.name, L"AngleSpeedKey", value, ini_path);
    }

    static void create_cfg_file(bool first_run)
    {
        char first_run_setinfos[CFG_MAX_COUNT];
        if (first_run) {
            std::snprintf(
                first_run_setinfos, std::size(first_run_setinfos), 1 + R"(
setinfo %s "%s"
setinfo %s "%s"
setinfo %s "%s"
setinfo %s "%s")",
                freq->name, freq->value,
                sleep->name, sleep->value,
                yawspeed->name, yawspeed->value,
                anglespeedkey->name, anglespeedkey->value);
        }

        char text[CFG_MAX_COUNT];
        auto count = std::snprintf(text, std::size(text), 1 + R"(
setinfo %s "%s"
setinfo %s "%s"

alias %s "con_logfile :; con_logfile; con_filter_enable 0; con_filter_enable"
alias +%s "toggle +_left_conturn;"
alias -%s "toggle -_left_conturn;"
alias +%s "toggle +_right_conturn;"
alias -%s "toggle -_right_conturn;"
alias +%s "toggle +_speed_conturn;"
alias -%s "toggle -_speed_conturn;"%s%s

con_logfile %S
con_filter_text_out "_conturn is not a valid cvar"
con_filter_enable 1

%s
%s
%s
%s
%s
%s
%s
%s

con_logfile
con_filter_text_out
con_filter_enable
)",

            version->name, version->value,
            url->name, url->value,

            off_alias_name,
            left_alias_name,
            left_alias_name,
            right_alias_name,
            right_alias_name,
            speed_alias_name,
            speed_alias_name,

            (first_run ? "\n\n": ""), (first_run ? first_run_setinfos : ""),

            con_log_filename,

            version->name,
            url->name,
            freq->name,
            sleep->name,
            yawspeed->name,
            anglespeedkey->name,
            sensitivity->name,
            yaw->name);

        HANDLE file = CreateFileW(cfg_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written;
        WriteFile(file, text, count * sizeof(*text), &written, nullptr);
        CloseHandle(file);
    }

    static void delete_cfg_file()
    {
        DeleteFileW(cfg_path);
    }

    static void create_con_log_file()
    {
        CreateSymbolicLinkW(con_log_path, pipe_path, 0);
    }

    static void delete_con_log_file()
    {
        DeleteFileW(con_log_path);
    }

    static void handle_con_line(const char *line)
    {
        if (0 == std::strcmp(line, R"(+_left_conturn is not a valid cvar)")) {
            std::strcpy(in_left->value, "1");
            in_left->parse_bool();
        } else if (0 == std::strcmp(line, R"(+_right_conturn is not a valid cvar)")) {
            std::strcpy(in_right->value, "1");
            in_right->parse_bool();
        } else if (0 == std::strcmp(line, R"(+_speed_conturn is not a valid cvar)")) {
            std::strcpy(in_speed->value, "1");
            in_speed->parse_bool();
        } else if (0 == std::strcmp(line, R"(-_left_conturn is not a valid cvar)")) {
            std::strcpy(in_left->value, "0");
            in_left->parse_bool();
        } else if (0 == std::strcmp(line, R"(-_right_conturn is not a valid cvar)")) {
            std::strcpy(in_right->value, "0");
            in_right->parse_bool();
        } else if (0 == std::strcmp(line, R"(-_speed_conturn is not a valid cvar)")) {
            std::strcpy(in_speed->value, "0");
            in_speed->parse_bool();
        } else if (yawspeed->parse_con_cvar_line(line)) {
            yawspeed->parse_double();
        } else if (anglespeedkey->parse_con_cvar_line(line)) {
            anglespeedkey->parse_double();
        } else if (sensitivity->parse_con_cvar_line(line)) {
            sensitivity->parse_double();
        } else if (yaw->parse_con_cvar_line(line)) {
            yaw->parse_double();
        } else if (freq->parse_con_cvar_line(line)) {
            freq->parse_double();
        } else if (sleep->parse_con_cvar_line(line)) {
            sleep->parse_double();
        } else if (0 == std::strncmp(line, R"("con_logfile" = ")", std::size(R"("con_logfile" = ")") - 1)) {
            create_cfg_file(false);
        }
    }

    inline static wchar_t image_path[PATHCCH_MAX_CCH];
    inline static VersionInfo version_info;
    inline static wchar_t pipe_path[PATHCCH_MAX_CCH];
    inline static wchar_t cfg_filename[PATHCCH_MAX_CCH];
    inline static wchar_t con_log_filename[PATHCCH_MAX_CCH];
    inline static std::optional<ConLogPipe> con_log_pipe;
    inline static wchar_t ini_path[PATHCCH_MAX_CCH];
    inline static wchar_t game_path[PATHCCH_MAX_CCH];
    inline static wchar_t cfg_path[PATHCCH_MAX_CCH];
    inline static wchar_t con_log_path[PATHCCH_MAX_CCH];
    inline static char off_alias_name[CON_VAR_MAX_COUNT];
    inline static char left_alias_name[CON_VAR_MAX_COUNT];
    inline static char right_alias_name[CON_VAR_MAX_COUNT];
    inline static char speed_alias_name[CON_VAR_MAX_COUNT];
    inline static std::optional<ConVar> version;
    inline static std::optional<ConVar> url;
    inline static std::optional<ConVar> freq;
    inline static std::optional<ConVar> sleep;
    inline static std::optional<ConVar> yawspeed;
    inline static std::optional<ConVar> anglespeedkey;
    inline static std::optional<ConVar> sensitivity;
    inline static std::optional<ConVar> yaw;
    inline static std::optional<ConVar> in_left;
    inline static std::optional<ConVar> in_right;
    inline static std::optional<ConVar> in_speed;
    inline static std::optional<TrayIcon> tray_icon;
};

int main(int argc, char *argv[])
{
    App::run();
    return 0;
}
