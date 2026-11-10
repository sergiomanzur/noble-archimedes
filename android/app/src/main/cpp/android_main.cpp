// Android: the host's window and main loop on SDL3, adapted from the Linux
// preview. SDL's Android backend runs this on its native thread; the Java
// activity owns the splash and touch overlay, and talks to the game through
// android_native.cpp.
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <unwind.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#include <imgui.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "linux/sdl_app.h"
#include "noble_android.h"
#include "windows.h"

namespace app {

namespace {

int g_argc = 1;
char* g_argv_storage[] = {const_cast<char*>("noble"), nullptr};
char** g_argv = g_argv_storage;
Uint32 g_apply_event = 0, g_open_event = 0;
std::mutex g_open_lock;
std::condition_variable g_open_cv;
uint64_t g_opened = 0;

bool HasArg(const char* arg) {
    for (int i = 1; i < g_argc; i++)
        if (!std::strcmp(g_argv[i], arg)) return true;
    return false;
}

// Android has no execinfo.h; libunwind's IPs plus dladdr give a usable host
// stack for llvm-symbolizer against the unstripped libmain.so.
struct HostTrace {
    void* frames[64];
    int count = 0;
};

_Unwind_Reason_Code TraceFrame(_Unwind_Context* ctx, void* arg) {
    auto* trace = static_cast<HostTrace*>(arg);
    uintptr_t ip = _Unwind_GetIP(ctx);
    if (ip && trace->count < 64) trace->frames[trace->count++] = reinterpret_cast<void*>(ip);
    return trace->count < 64 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

void LogHostTrace() {
    HostTrace trace;
    _Unwind_Backtrace(TraceFrame, &trace);
    for (int i = 0; i < trace.count; i++) {
        Dl_info dl{};
        if (dladdr(trace.frames[i], &dl) && dl.dli_fbase) {
            uintptr_t off = reinterpret_cast<uintptr_t>(trace.frames[i]) - reinterpret_cast<uintptr_t>(dl.dli_fbase);
            LOGE("  host #%02d %s+0x%llx", i, dl.dli_fname ? dl.dli_fname : "?", (unsigned long long)off);
        } else {
            LOGE("  host #%02d %p", i, trace.frames[i]);
        }
    }
}

// Guest registers and return addresses into the log, then a host stack.
void Crash(int sig, siginfo_t* info, void*) {
    static std::atomic<bool> once{false};
    if (!once.exchange(true)) {
        if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
            uint64_t va = uint64_t(reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr));
            uint64_t g = uint64_t(reinterpret_cast<uintptr_t>(kernel::g_base));
            LOGE("signal %d at %llX on guest thread %X, guest %08llX", sig, (unsigned long long)va, t->id,
                 (unsigned long long)(va - g));
            kernel::LogGuestStack(*t->ctx, 256);
        } else {
            LOGE("signal %d", sig);
        }
        LogHostTrace();
        logx::Flush();
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void DumpThreads() {
    gpu::g_capture = true;
    for (auto& t : kernel::Threads())
        if (t->ctx) {
            LOGI("thread %X", t->id);
            kernel::LogGuestStack(*t->ctx, 32);
        }
}

// Android is always full-screen; the desktop mode settings do not apply.
void ApplyNow() {
    SDL_Window* w = linuxapp::g_window;
    if (!w) return;
    SDL_SetWindowFullscreenMode(w, nullptr);
    SDL_SetWindowFullscreen(w, true);
    SDL_SyncWindow(w);
}

void UpdatePixelSize() {
    int w = 0, h = 0;
    if (SDL_Window* win = linuxapp::g_window) SDL_GetWindowSizeInPixels(win, &w, &h);
    linuxapp::g_pixel_w = uint32_t(std::max(w, 1));
    linuxapp::g_pixel_h = uint32_t(std::max(h, 1));
}

bool Message(uint32_t msg, uintptr_t wp, intptr_t lp) {
    intptr_t result = 0;
    return kGame.message && kGame.message(linuxapp::g_window.load(), msg, wp, lp, &result);
}

bool Handle(const SDL_Event& e) {
    {
        std::lock_guard lock(UiLock());
        linuxapp::ImGuiEvent(e);
    }
    if (e.type == g_apply_event) {
        ApplyNow();
        UpdatePixelSize();
        return false;
    }
    if (e.type == g_open_event) {
        CloseSplash();
        if (e.user.code) ApplyNow();
        SDL_ShowWindow(linuxapp::g_window);
        SDL_RaiseWindow(linuxapp::g_window);
        UpdatePixelSize();
        std::lock_guard lock(g_open_lock);
        g_opened++;
        g_open_cv.notify_all();
        return false;
    }
    switch (e.type) {
        case SDL_EVENT_QUIT:
            return true;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            Message(WM_CLOSE, 0, 0);
            return true;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            linuxapp::g_focused = true;
            Message(WM_ACTIVATE, WA_ACTIVE, 0);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            linuxapp::g_focused = false;
            Message(WM_ACTIVATE, WA_INACTIVE, 0);
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED:
            UpdatePixelSize();
            break;
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            // The Java activity releases the touch pad in onPause; SDL pauses
            // audio and blocks this loop while backgrounded.
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            ApplyNow();
            UpdatePixelSize();
#ifdef __ANDROID__
            // The activity may have replaced the native window while backgrounded; Present
            // makes the Vulkan surface and swap chain again before it draws. Queued touch
            // events from the old surface can otherwise trip ImGui's input processing.
            {
                std::lock_guard lock(UiLock());
                ImGuiIO& io = ImGui::GetIO();
                io.ClearEventsQueue();
                io.ClearInputKeys();
                io.ClearInputMouse();
            }
            gpu::g_surface_dirty = true;
#endif
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            int vk = linuxapp::VirtualKey(e.key.scancode);
            if (!vk) break;
            bool down = e.type == SDL_EVENT_KEY_DOWN, alt = (e.key.mod & SDL_KMOD_ALT) != 0;
            uint32_t msg = down ? (alt ? WM_SYSKEYDOWN : WM_KEYDOWN) : (alt ? WM_SYSKEYUP : WM_KEYUP);
            intptr_t lp = intptr_t(uint32_t(vk) << 16 | (e.key.repeat ? 1u << 30 : 0) | (down ? 0 : 3u << 30));
            if (Message(msg, uintptr_t(vk), lp)) break;
            if (down && vk == VK_F3) DumpThreads();
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT)
                Message(WM_LBUTTONUP, 0, intptr_t(int(e.button.x) & 0xFFFF) | intptr_t(int(e.button.y) & 0xFFFF) << 16);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            input::PadAdded(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            input::PadRemoved(e.gdevice.which);
            break;
        default:
            break;
    }
    return false;
}

uint32_t RefreshRate() {
    static std::atomic<uint32_t> rate{0};
    static std::atomic<uint64_t> next{0};
    uint64_t now = SDL_GetTicks();
    if (now >= next) {
        next = now + 1000;
        SDL_Window* w = linuxapp::g_window;
        const SDL_DisplayMode* m = w ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w)) : nullptr;
        rate = m && m->refresh_rate > 1 ? uint32_t(m->refresh_rate + 0.5f) : 0;
    }
    return rate;
}

}  // namespace

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = [] {
        const char* p = SDL_GetAndroidExternalStoragePath();
        std::filesystem::path path;
        if (p && *p) {
            path = p;
        } else {
            // The package's private files directory is the last resort; normally
            // SDL gives us /sdcard/Android/data/<package>/files.
            path = "/data/data/com.noblearchimedes.game/files";
        }
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        std::filesystem::create_directories(path / "logs", ec);
        std::filesystem::create_directories(path / "assets", ec);
        return path;
    }();
    return dir;
}

int Instance() { return 0; }
void* Window() { return linuxapp::g_window.load(); }
Mode WindowPixels() { return {linuxapp::g_pixel_w, linuxapp::g_pixel_h}; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() {
    SDL_Event e{};
    e.type = g_apply_event;
    SDL_PushEvent(&e);
}

void OpenGameWindow(bool apply) {
    std::unique_lock lock(g_open_lock);
    uint64_t before = g_opened;
    SDL_Event e{};
    e.type = g_open_event;
    e.user.code = apply ? 1 : 0;
    SDL_PushEvent(&e);
    g_open_cv.wait_for(lock, std::chrono::seconds(5), [&] { return g_opened != before; });
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    if (cap == kFpsCapRefresh) cap = RefreshRate();
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / (uint64_t(cap) * Speed()));
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

int Argc() { return g_argc; }
char** Argv() { return g_argv; }

}  // namespace app

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    struct sigaction sa{};
    sa.sa_sigaction = app::Crash;
    sa.sa_flags = SA_SIGINFO;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    // One log a session, under the app's external files directory.
    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log, false);
#ifdef __ANDROID__
    // vkd3d's diagnostics go to stderr; keep them beside the game for bring-up.
    setenv("VKD3D_DEBUG", "warn", 1);
    static FILE* vkd3d_log = freopen((app::ExeDir() / "vkd3d.log").string().c_str(), "w", stderr);
    (void)vkd3d_log;
#endif
    app::Load();
    if (app::kGame.init) app::kGame.init();

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "1");
    // SDL's touch-to-mouse bridge feeds the ImGui menu: one pointer with mouse motion and a left button, so taps
    // choose settings and drags can scroll the page (menu.cpp).
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    SDL_SetHint(SDL_HINT_APP_NAME, linuxapp::Utf8(app::kGame.title).c_str());
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        LOGE("SDL failed to start: %s", SDL_GetError());
        return 1;
    }
    app::g_apply_event = SDL_RegisterEvents(2);
    app::g_open_event = app::g_apply_event + 1;
    app::LogSystem();
    LOGR("Video: %s", SDL_GetCurrentVideoDriver());
    LOGR("Storage: %s", app::ExeDir().string().c_str());

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;
    app::MakeWindowIcon(game);

    std::string title = linuxapp::Utf8(app::kGame.title);
    SDL_Window* window = SDL_CreateWindow(title.c_str(), 1280, 720,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        LOGE("window: %s", SDL_GetError());
        MessageBoxW(nullptr, linuxapp::Wide(std::string("The game window could not be opened: ") + SDL_GetError()).c_str(),
                    app::kGame.short_title, MB_ICONERROR);
        return 1;
    }
    linuxapp::g_window = window;
    app::ApplyWindowIcon(window);
    app::ApplyNow();
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);
    app::UpdatePixelSize();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    linuxapp::ImGuiInit(window);

    input::Init();
    if (app::kGame.window) app::kGame.window(window);
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / "hdd").string())) {
        MessageBoxW(nullptr, L"The game failed to start, see the log in the logs folder.", app::kGame.short_title,
                    MB_ICONERROR);
        return 1;
    }
    for (bool closing = false; !closing;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 4)) {
            closing = app::Handle(e);
            while (!closing && SDL_PollEvent(&e)) closing = app::Handle(e);
        }
        linuxapp::ApplyCursor();
        bool text = ImGui::GetIO().WantTextInput;
        if (text != SDL_TextInputActive(window)) text ? SDL_StartTextInput(window) : SDL_StopTextInput(window);
    }
    logx::Flush();
    kernel::Shutdown(0);
    return 0;
}
