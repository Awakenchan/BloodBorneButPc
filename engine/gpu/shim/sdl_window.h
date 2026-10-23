#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include "common/types.h"

struct SDL_Window;

namespace Frontend {

bool WindowInBackground();
u32 BackgroundFrameLimit();
u32 DisplayRefreshHz();

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();
    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    SDL_Window* GetSDLWindow() const { return window; }
    WindowSystemInfo GetWindowInfo() const { return window_info; }
    bool IsOpen() const { return is_open.load(std::memory_order_relaxed); }
    bool PollEvents();
    void BeginTextInput(const std::string& initial, const std::string& prompt);
    int PollTextInput(std::string& text);

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::mutex text_mutex;
    bool text_requested{}, text_active{};
    bool text_selected{};
    int text_state{};
    std::string text, text_prompt, base_title;
    void UpdateTextTitle();
    void UpdateCursor();
    u64 last_mouse_motion_ms{};
    bool cursor_hidden{};
    SDL_Window* window{};
    WindowSystemInfo window_info{};
#ifdef _WIN32
    void* win_icon_big{};
    void* win_icon_small{};
#endif
};

}
