// Windows backend: a per-user low-level mouse hook (no driver, no admin
// rights) that swallows button chatter and wheel bounce from physical input
// and re-injects corrected events with SendInput.
//
// Injected input from other software (remote desktop, automation tools, and
// our own corrections) is never touched.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "mouse-filter.hpp"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

constexpr wchar_t kWindowClass[] = L"MouseHardwareFixWindow";
constexpr wchar_t kMutexName[] = L"Local\\MouseHardwareFix";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"MouseHardwareFix";
constexpr UINT kMsgInjectWheel = WM_APP + 1;

enum Button { kLeft, kRight, kMiddle, kX1, kX2, kButtonCount };

struct State {
    mhf::Options opt;
    mhf::ScrollFilter scroll;
    mhf::ButtonDebouncer buttons[kButtonCount];
    HANDLE timer = nullptr;
    HWND window = nullptr;
    LARGE_INTEGER qpc_freq = {};
} g;

mhf::Micros now_us() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<mhf::Micros>(t.QuadPart / g.qpc_freq.QuadPart * 1000000 +
                                    t.QuadPart % g.qpc_freq.QuadPart * 1000000 / g.qpc_freq.QuadPart);
}

void log(const char* fmt, ...) {
    if (!g.opt.verbose) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fflush(stderr);
}

void send_release(int button) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    switch (button) {
        case kLeft: in.mi.dwFlags = MOUSEEVENTF_LEFTUP; break;
        case kRight: in.mi.dwFlags = MOUSEEVENTF_RIGHTUP; break;
        case kMiddle: in.mi.dwFlags = MOUSEEVENTF_MIDDLEUP; break;
        case kX1: in.mi.dwFlags = MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON1; break;
        case kX2: in.mi.dwFlags = MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON2; break;
    }
    SendInput(1, &in, sizeof(in));
}

void send_wheel(int delta) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(delta);
    SendInput(1, &in, sizeof(in));
}

// Arms the waitable timer for the earliest pending release.
void rearm_timer() {
    mhf::Micros next = mhf::kNoDeadline;
    for (const auto& b : g.buttons)
        if (b.deadline() < next) next = b.deadline();
    if (next == mhf::kNoDeadline) {
        CancelWaitableTimer(g.timer);
        return;
    }
    mhf::Micros wait = next - now_us();
    LARGE_INTEGER due;
    due.QuadPart = -(wait > 0 ? wait * 10 : 1);  // relative, in 100 ns units
    SetWaitableTimer(g.timer, &due, 0, nullptr, nullptr, FALSE);
}

void fire_due_releases() {
    mhf::Micros now = now_us();
    for (int i = 0; i < kButtonCount; ++i)
        if (g.buttons[i].poll(now)) send_release(i);
    rearm_timer();
}

// Returns true if the event must be swallowed.
bool filter_button(int button, bool down) {
    auto& b = g.buttons[button];
    mhf::Micros t = now_us();
    if (down) {
        unsigned long before = b.suppressed();
        bool pass = b.on_press(t);
        if (b.suppressed() != before) log("button %d chatter suppressed\n", button);
        rearm_timer();
        return !pass;
    }
    bool pass = b.on_release(t);
    rearm_timer();
    return !pass;
}

LRESULT CALLBACK mouse_hook(int code, WPARAM wparam, LPARAM lparam) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wparam, lparam);
    const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
    if (ms->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED))
        return CallNextHookEx(nullptr, code, wparam, lparam);

    bool swallow = false;
    if (g.opt.click_fix) {
        switch (wparam) {
            case WM_LBUTTONDOWN: swallow = filter_button(kLeft, true); break;
            case WM_LBUTTONUP: swallow = filter_button(kLeft, false); break;
            case WM_RBUTTONDOWN: swallow = filter_button(kRight, true); break;
            case WM_RBUTTONUP: swallow = filter_button(kRight, false); break;
            case WM_MBUTTONDOWN: swallow = filter_button(kMiddle, true); break;
            case WM_MBUTTONUP: swallow = filter_button(kMiddle, false); break;
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP: {
                int button = HIWORD(ms->mouseData) == XBUTTON1 ? kX1 : kX2;
                swallow = filter_button(button, wparam == WM_XBUTTONDOWN);
                break;
            }
        }
    }

    if (g.opt.scroll_fix && wparam == WM_MOUSEWHEEL) {
        int delta = static_cast<short>(HIWORD(ms->mouseData));
        auto verdict = g.scroll.on_scroll(now_us(), delta);
        if (verdict != mhf::ScrollFilter::Verdict::Pass) {
            swallow = true;
            bool invert = verdict == mhf::ScrollFilter::Verdict::Invert;
            log("scroll bounce %+d %s\n", delta, invert ? "inverted" : "dropped");
            // Inject after the hook returns; hooks must stay fast.
            if (invert) PostMessage(g.window, kMsgInjectWheel, static_cast<WPARAM>(-delta), 0);
        }
    }

    return swallow ? 1 : CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case kMsgInjectWheel:
            send_wheel(static_cast<int>(static_cast<INT_PTR>(wparam)));
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

// Prints to the console we were started from, if any (this is a GUI-subsystem
// program so that autostart does not open a console window).
void attach_console() {
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN) return;  // redirected
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
}

void usage() {
    std::fprintf(stderr,
        "Usage: mouse-hardware-fix [options]\n"
        "  --disable-scroll          do not filter wheel bounce\n"
        "  --disable-click           do not filter button chatter\n"
        "  --scroll-timeout <sec>    pause that ends a scroll gesture (default 0.300)\n"
        "  --scroll-reversal <n>     notches needed to accept a mid-gesture reversal (default 2)\n"
        "  --scroll-drop             drop bounced wheel steps instead of inverting them\n"
        "  --click-timeout <sec>     chatter window for buttons (default 0.025)\n"
        "  --verbose                 log every corrected event to the console\n"
        "\n"
        "  --install                 start at login with the given options, and start now\n"
        "  --uninstall               remove from login and stop the running instance\n"
        "  --stop                    stop the running instance\n");
}

bool stop_running_instance() {
    HWND w = FindWindowEx(HWND_MESSAGE, nullptr, kWindowClass, nullptr);
    if (!w) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessage(w, WM_CLOSE, 0, 0);
    if (proc) {
        WaitForSingleObject(proc, 3000);
        CloseHandle(proc);
    }
    return true;
}

std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }

int install(const std::wstring& args) {
    wchar_t exe[MAX_PATH];
    GetModuleFileName(nullptr, exe, MAX_PATH);
    std::wstring cmd = quote(exe) + args;

    HKEY key;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        std::fprintf(stderr, "Cannot open the Run registry key\n");
        return 1;
    }
    LSTATUS r = RegSetValueEx(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                              static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) {
        std::fprintf(stderr, "Cannot write the Run registry value\n");
        return 1;
    }

    stop_running_instance();
    STARTUPINFO si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (CreateProcess(nullptr, &cmd[0], nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    std::fprintf(stderr, "Installed: %ls\n", cmd.c_str());
    return 0;
}

int uninstall() {
    HKEY key;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValue(key, kRunValue);
        RegCloseKey(key);
    }
    stop_running_instance();
    std::fprintf(stderr, "Uninstalled\n");
    return 0;
}

bool parse_args(int argc, wchar_t** argv, std::wstring& forwarded, int& action) {
    // action: 0 = run, 1 = install, 2 = uninstall, 3 = stop, 4 = help
    auto narrow = [](const wchar_t* w) {
        std::string s;
        for (; *w; ++w) s += static_cast<char>(*w < 128 ? *w : '?');
        return s;
    };
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        bool takes_value = a == L"--scroll-timeout" || a == L"--click-timeout" || a == L"--scroll-reversal";
        std::string value;
        if (takes_value) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %ls\n", a.c_str());
                return false;
            }
            value = narrow(argv[++i]);
        }

        bool ok = true;
        if (a == L"--install") action = 1;
        else if (a == L"--uninstall") action = 2;
        else if (a == L"--stop") action = 3;
        else if (a == L"--help" || a == L"-h") action = 4;
        else if (a == L"--disable-scroll") g.opt.scroll_fix = false;
        else if (a == L"--disable-click") g.opt.click_fix = false;
        else if (a == L"--scroll-drop") g.opt.scroll.invert = false;
        else if (a == L"--verbose") g.opt.verbose = true;
        else if (a == L"--scroll-timeout") ok = mhf::parse_seconds(value.c_str(), g.opt.scroll.idle_reset);
        else if (a == L"--click-timeout") ok = mhf::parse_seconds(value.c_str(), g.opt.click_window);
        else if (a == L"--scroll-reversal") ok = mhf::parse_notches(value.c_str(), g.opt.scroll.reversal_units);
        else {
            std::fprintf(stderr, "Unknown option %ls\n", a.c_str());
            return false;
        }
        if (!ok) {
            std::fprintf(stderr, "Invalid value for %ls: %s\n", a.c_str(), value.c_str());
            return false;
        }

        if (a != L"--install" && a != L"--uninstall" && a != L"--stop" && a != L"--verbose") {
            forwarded += L" " + a;
            if (takes_value) forwarded += L" " + std::wstring(argv[i]);
        }
    }
    return true;
}

int run() {
    HANDLE mutex = CreateMutex(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::fprintf(stderr, "mouse-hardware-fix is already running (use --stop first)\n");
        return 1;
    }

    QueryPerformanceFrequency(&g.qpc_freq);
    g.scroll.set_config(g.opt.scroll);
    for (auto& b : g.buttons) b.set_window(g.opt.click_window);

    // High-resolution timer (Windows 10 1803+), so deferred releases fire
    // within ~1 ms instead of on the 15.6 ms system tick.
    g.timer = CreateWaitableTimerEx(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g.timer) g.timer = CreateWaitableTimer(nullptr, FALSE, nullptr);

    HINSTANCE inst = GetModuleHandle(nullptr);
    WNDCLASS wc = {};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = inst;
    wc.lpszClassName = kWindowClass;
    RegisterClass(&wc);
    g.window = CreateWindowEx(0, kWindowClass, L"mouse-hardware-fix", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);

    // Input processing is latency sensitive; the hook runs on this thread.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    HHOOK hook = SetWindowsHookEx(WH_MOUSE_LL, mouse_hook, inst, 0);
    if (!hook) {
        std::fprintf(stderr, "SetWindowsHookEx failed: %lu\n", GetLastError());
        return 1;
    }
    std::fprintf(stderr, "mouse-hardware-fix running | scroll fix: %s | click fix: %s\n",
                 g.opt.scroll_fix ? "on" : "off", g.opt.click_fix ? "on" : "off");
    std::fflush(stderr);

    bool running = true;
    while (running) {
        DWORD r = MsgWaitForMultipleObjectsEx(1, &g.timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0) fire_due_releases();
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    UnhookWindowsHookEx(hook);
    // Never leave a button stuck down.
    for (int i = 0; i < kButtonCount; ++i)
        if (g.buttons[i].deadline() != mhf::kNoDeadline) send_release(i);
    CloseHandle(g.timer);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    attach_console();

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring forwarded;
    int action = 0;
    bool ok = parse_args(argc, argv, forwarded, action);
    LocalFree(argv);
    if (!ok) {
        usage();
        return 1;
    }

    switch (action) {
        case 1: return install(forwarded);
        case 2: return uninstall();
        case 3:
            if (!stop_running_instance()) std::fprintf(stderr, "Not running\n");
            return 0;
        case 4: usage(); return 0;
    }
    if (!g.opt.scroll_fix && !g.opt.click_fix) {
        std::fprintf(stderr, "Both fixes disabled, nothing to do.\n");
        return 0;
    }
    return run();
}
