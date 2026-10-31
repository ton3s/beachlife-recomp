#include "ansi_text.h"
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include "compat_host.h"
#include "input_map.h"
#include "platform/video.h"
#include "user32_cursor.h"
#include "user32_host.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>

namespace
{
using beachlife::compat::unsupported;
HWND current_window = nullptr;
SDL_WindowID current_id = 0;
WNDPROC window_procedure = nullptr;
int cursor_count = 0;
HCURSOR selected_cursor = nullptr;
bool cursor_initialized = false;
std::array<SDL_Cursor*, SDL_SYSTEM_CURSOR_COUNT> system_cursors{};
std::array<bool, 256> pressed{};
std::deque<MSG> messages;
std::mutex message_mutex;
bool queue_enabled = false;
bool text_input_enabled = false;
bool system_key = false;
bool events_installed = false;
bool window_alive = false;
bool keep_hidden = false;
void (*prepare_frame)(void*) = nullptr;
void* frame_context = nullptr;
WNDCLASSEXA registered_class{};
std::string class_name;
int application_icon;

int virtual_key(SDL_Scancode scan)
{
    if (scan >= SDL_SCANCODE_A && scan <= SDL_SCANCODE_Z)
    {
        return 'A' + scan - SDL_SCANCODE_A;
    }
    if (scan >= SDL_SCANCODE_1 && scan <= SDL_SCANCODE_9)
    {
        return '1' + scan - SDL_SCANCODE_1;
    }
    if (scan >= SDL_SCANCODE_F1 && scan <= SDL_SCANCODE_F12)
    {
        return 0x70 + scan - SDL_SCANCODE_F1;
    }
    if (scan >= SDL_SCANCODE_KP_1 && scan <= SDL_SCANCODE_KP_9)
    {
        return 0x61 + scan - SDL_SCANCODE_KP_1;
    }
    switch (scan)
    {
    case SDL_SCANCODE_0:
        return '0';
    case SDL_SCANCODE_BACKSPACE:
        return 8;
    case SDL_SCANCODE_TAB:
        return 9;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        return 13;
    case SDL_SCANCODE_LSHIFT:
        return 0xa0;
    case SDL_SCANCODE_RSHIFT:
        return 0xa1;
    case SDL_SCANCODE_LCTRL:
        return 0xa2;
    case SDL_SCANCODE_RCTRL:
        return 0xa3;
    case SDL_SCANCODE_LALT:
        return 0xa4;
    case SDL_SCANCODE_RALT:
        return 0xa5;
    case SDL_SCANCODE_PAUSE:
        return 0x13;
    case SDL_SCANCODE_CAPSLOCK:
        return 0x14;
    case SDL_SCANCODE_ESCAPE:
        return 0x1b;
    case SDL_SCANCODE_SPACE:
        return 0x20;
    case SDL_SCANCODE_PAGEUP:
        return 0x21;
    case SDL_SCANCODE_PAGEDOWN:
        return 0x22;
    case SDL_SCANCODE_END:
        return 0x23;
    case SDL_SCANCODE_HOME:
        return 0x24;
    case SDL_SCANCODE_LEFT:
        return 0x25;
    case SDL_SCANCODE_UP:
        return 0x26;
    case SDL_SCANCODE_RIGHT:
        return 0x27;
    case SDL_SCANCODE_DOWN:
        return 0x28;
    case SDL_SCANCODE_PRINTSCREEN:
        return 0x2c;
    case SDL_SCANCODE_INSERT:
        return 0x2d;
    case SDL_SCANCODE_DELETE:
        return 0x2e;
    case SDL_SCANCODE_KP_0:
        return 0x60;
    case SDL_SCANCODE_KP_MULTIPLY:
        return 0x6a;
    case SDL_SCANCODE_KP_PLUS:
        return 0x6b;
    case SDL_SCANCODE_KP_MINUS:
        return 0x6d;
    case SDL_SCANCODE_KP_PERIOD:
        return 0x6e;
    case SDL_SCANCODE_KP_DIVIDE:
        return 0x6f;
    case SDL_SCANCODE_NUMLOCKCLEAR:
        return 0x90;
    case SDL_SCANCODE_SCROLLLOCK:
        return 0x91;
    case SDL_SCANCODE_SEMICOLON:
        return 0xba;
    case SDL_SCANCODE_EQUALS:
        return 0xbb;
    case SDL_SCANCODE_COMMA:
        return 0xbc;
    case SDL_SCANCODE_MINUS:
        return 0xbd;
    case SDL_SCANCODE_PERIOD:
        return 0xbe;
    case SDL_SCANCODE_SLASH:
        return 0xbf;
    case SDL_SCANCODE_GRAVE:
        return 0xc0;
    case SDL_SCANCODE_LEFTBRACKET:
        return 0xdb;
    case SDL_SCANCODE_BACKSLASH:
        return 0xdc;
    case SDL_SCANCODE_RIGHTBRACKET:
        return 0xdd;
    case SDL_SCANCODE_APOSTROPHE:
        return 0xde;
    default:
        return 0;
    }
}

bool SDLCALL watch_event(void*, SDL_Event* event)
{
    try
    {
        std::lock_guard lock(message_mutex);
        MSG message{};
        message.hwnd = current_window;
        message.time = static_cast<DWORD>(event->common.timestamp / 1000000);
        SDL_WindowID id = 0;
        switch (event->type)
        {
        case SDL_EVENT_TEXT_INPUT:
        {
            if (!queue_enabled || !window_alive || event->text.windowID != current_id)
            {
                return true;
            }
            std::string text;
            if (!beachlife::compat::utf8_to_ansi(event->text.text, text))
            {
                unsupported("TranslateMessage", "text cannot be represented in Windows-1252");
            }
            message.message = system_key ? 0x106 : 0x102; // WM_SYSCHAR / WM_CHAR
            message.lParam = 1;
            for (const unsigned char byte : text)
            {
                message.wParam = byte;
                messages.push_back(message);
            }
            return true;
        }
        case SDL_EVENT_QUIT:
            message.message = WM_CLOSE;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            message.message = WM_CLOSE;
            id = event->window.windowID;
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            message.message = WM_ACTIVATEAPP;
            message.wParam = event->type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
                             SDL_getenv("BEACHLIFE_BOOT_ASSUME_FOCUS");
            id = event->window.windowID;
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
            message.message = 0xf;
            id = event->window.windowID;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        {
            const int key = event->button.button == SDL_BUTTON_LEFT     ? 1
                            : event->button.button == SDL_BUTTON_RIGHT  ? 2
                            : event->button.button == SDL_BUTTON_MIDDLE ? 4
                            : event->button.button == SDL_BUTTON_X1     ? 5
                                                                        : 6;
            pressed[key] = true;
            return true;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        {
            const int key = virtual_key(event->key.scancode);
            if (!key)
            {
                return true;
            }
            if (event->key.down && !event->key.repeat)
            {
                pressed[key] = true;
                if (key >= 0xa0 && key <= 0xa5)
                {
                    pressed[0x10 + (key - 0xa0) / 2] = true;
                }
            }
            message.message = (event->key.mod & SDL_KMOD_ALT) ? 0x104 : 0x100;
            system_key = event->key.mod & SDL_KMOD_ALT;
            if (!event->key.down)
            {
                ++message.message;
            }
            message.wParam = key;
            message.lParam = 1 |
                             (static_cast<LPARAM>(event->key.repeat || !event->key.down) << 30) |
                             (static_cast<LPARAM>(!event->key.down) << 31);
            id = event->key.windowID;
            break;
        }
        default:
            return true;
        }
        if (id && id != current_id)
        {
            return true;
        }
        if (queue_enabled && window_alive)
        {
            messages.push_back(message);
#if defined(TARGET_OS_IOS) && TARGET_OS_IOS
            if (event->type == SDL_EVENT_KEY_DOWN && text_input_enabled)
            {
                const SDL_Keycode typed =
                    SDL_GetKeyFromScancode(event->key.scancode, event->key.mod, false);
                if ((typed >= 0x20 && typed < 0x7f) || typed == 8 || typed == 13)
                {
                    message.message = 0x102; // WM_CHAR
                    message.wParam = static_cast<WPARAM>(typed);
                    message.lParam = 1;
                    messages.push_back(message);
                }
            }
#endif
        }
    }
    catch (...)
    {
        unsupported("USER32 event queue", "cannot retain an SDL event");
    }
    return true;
}

void start_events()
{
    if (!events_installed && !SDL_AddEventWatch(watch_event, nullptr))
    {
        unsupported("USER32 event queue", SDL_GetError());
    }
    events_installed = true;
}

SDL_Window* window(HWND handle, const char* api)
{
    if (!SDL_IsMainThread())
    {
        unsupported(api, "window operation outside SDL's main thread");
    }
    if (!handle)
    {
        return nullptr;
    }
    SDL_Window* target = beachlife::compat::video(handle)->window();
    if (!target)
    {
        return nullptr;
    }
    {
        std::lock_guard lock(message_mutex);
        current_window = handle;
        current_id = SDL_GetWindowID(target);
    }
    beachlife::compat::input::bind(handle);
    start_events();
    return target;
}

struct Borders
{
    int top = 0, left = 0, bottom = 0, right = 0;
};
bool borders(SDL_Window* target, Borders& result)
{
    if (SDL_GetWindowFlags(target) & (SDL_WINDOW_BORDERLESS | SDL_WINDOW_FULLSCREEN))
    {
        return true;
    }
    return SDL_GetWindowBordersSize(
        target, &result.top, &result.left, &result.bottom, &result.right);
}
bool coordinate(std::int64_t value)
{
    return value >= std::numeric_limits<LONG>::min() && value <= std::numeric_limits<LONG>::max();
}
void init_cursor()
{
    if (!cursor_initialized)
    {
        selected_cursor = reinterpret_cast<HCURSOR>(SDL_GetCursor());
        cursor_initialized = true;
    }
}
void start_text_input()
{
    if (current_window && !text_input_enabled)
    {
#if !(defined(TARGET_OS_IOS) && TARGET_OS_IOS)
        // On iOS, starting text input always raises the on-screen keyboard over the
        // game. The event watch makes typed characters from key presses instead.
        SDL_Window* target = beachlife::compat::video(current_window)->window();
        if (!SDL_StartTextInput(target))
        {
            unsupported("TranslateMessage", SDL_GetError());
        }
#endif
        text_input_enabled = true;
    }
}
} // namespace

namespace beachlife::compat
{
HCURSOR current_cursor()
{
    init_cursor();
    return selected_cursor;
}

void bind_window(HWND handle)
{
    if (!window(handle, "bind_window"))
    {
        unsupported("bind_window", "missing host window");
    }
}

void bind_window_procedure(HWND handle, WNDPROC procedure)
{
    if (!window(handle, "bind_window_procedure"))
    {
        unsupported("bind_window_procedure", "missing host window");
    }
    window_procedure = procedure;
    start_text_input();
    std::lock_guard lock(message_mutex);
    queue_enabled = true;
    window_alive = true;
}

WindowSession::WindowSession(HWND handle, void (*before_poll)(void*), void* context, bool hidden)
{
    bind_window(handle);
    prepare_frame = before_poll;
    frame_context = context;
    keep_hidden = hidden;
}

WindowSession::~WindowSession()
{
    if (text_input_enabled)
    {
        SDL_StopTextInput(video(current_window)->window());
    }
    if (events_installed)
    {
        SDL_RemoveEventWatch(watch_event, nullptr);
    }
    std::lock_guard lock(message_mutex);
    messages.clear();
    pressed.fill(false);
    current_window = nullptr;
    current_id = 0;
    window_procedure = nullptr;
    registered_class = {};
    class_name.clear();
    queue_enabled = text_input_enabled = system_key = events_installed = window_alive = false;
    keep_hidden = false;
    prepare_frame = nullptr;
    frame_context = nullptr;
}
} // namespace beachlife::compat

extern "C"
{
    BOOL WINAPI SetRect(LPRECT rectangle, int left, int top, int right, int bottom)
    {
        if (!rectangle)
        {
            return FALSE;
        }
        *rectangle = {left, top, right, bottom};
        return TRUE;
    }
    BOOL WINAPI GetClientRect(HWND handle, LPRECT rectangle)
    {
        SDL_Window* target = window(handle, "GetClientRect");
        beachlife::compat::input::Mapping map;
        if (!target || !rectangle || !beachlife::compat::input::mapping(handle, map))
        {
            return FALSE;
        }
        return SetRect(rectangle, 0, 0, map.presentation.width, map.presentation.height);
    }
    BOOL WINAPI GetWindowRect(HWND handle, LPRECT rectangle)
    {
        SDL_Window* target = window(handle, "GetWindowRect");
        Borders frame;
        beachlife::compat::input::Mapping map;
        if (!target || !rectangle || !borders(target, frame) ||
            !beachlife::compat::input::mapping(handle, map))
        {
            return FALSE;
        }
        const std::int64_t l = std::int64_t(map.origin.x) - frame.left,
                           t = std::int64_t(map.origin.y) - frame.top,
                           r = std::int64_t(map.origin.x) + map.presentation.width + frame.right,
                           b = std::int64_t(map.origin.y) + map.presentation.height + frame.bottom;
        if (!coordinate(l) || !coordinate(t) || !coordinate(r) || !coordinate(b))
        {
            return FALSE;
        }
        return SetRect(rectangle, l, t, r, b);
    }
    BOOL WINAPI ClientToScreen(HWND handle, LPPOINT point)
    {
        SDL_Window* target = window(handle, "ClientToScreen");
        beachlife::compat::input::Mapping map;
        if (!target || !point || !beachlife::compat::input::mapping(handle, map))
        {
            return FALSE;
        }
        const std::int64_t sx = std::int64_t(map.origin.x) + point->x,
                           sy = std::int64_t(map.origin.y) + point->y;
        if (!coordinate(sx) || !coordinate(sy))
        {
            return FALSE;
        }
        *point = {static_cast<LONG>(sx), static_cast<LONG>(sy)};
        return TRUE;
    }

    BOOL WINAPI SetWindowPos(
        HWND handle, HWND after, int x, int y, int width, int height, UINT flags)
    {
        SDL_Window* target = window(handle, "SetWindowPos");
        if (!target)
        {
            return FALSE;
        }
#if defined(TARGET_OS_IOS) && TARGET_OS_IOS
        // The system owns an iOS window's size, position and stacking.
        return TRUE;
#endif
        // FRAMECHANGED and deferred/asynchronous positioning need another boundary.
        if (flags & ~UINT(0x07df))
        {
            unsupported("SetWindowPos", "unsupported positioning flags");
        }
        Borders frame;
        if (!borders(target, frame))
        {
            return FALSE;
        }
        if (!(flags & 4))
        {
            const auto order = reinterpret_cast<std::intptr_t>(after);
            if (order == -1 || order == -2)
            {
                if (!SDL_SetWindowAlwaysOnTop(target, order == -1))
                {
                    return FALSE;
                }
            }
            else if (order == 0)
            {
                if (!(flags & 0x10) && !SDL_RaiseWindow(target))
                {
                    return FALSE;
                }
            }
            else
            {
                unsupported("SetWindowPos", "relative window stacking");
            }
        }
        if (!(flags & 1))
        {
            const std::int64_t w = std::int64_t(width) - frame.left - frame.right,
                               h = std::int64_t(height) - frame.top - frame.bottom;
            if (w <= 0 || h <= 0 || !coordinate(w) || !coordinate(h) ||
                !SDL_SetWindowSize(target, w, h))
            {
                return FALSE;
            }
        }
        if (!(flags & 2))
        {
            const std::int64_t cx = std::int64_t(x) + frame.left, cy = std::int64_t(y) + frame.top;
            if (!coordinate(cx) || !coordinate(cy) || !SDL_SetWindowPosition(target, cx, cy))
            {
                return FALSE;
            }
        }
        if ((flags & 0x40) && !keep_hidden && !SDL_ShowWindow(target))
        {
            return FALSE;
        }
        if ((flags & 0x80) && !SDL_HideWindow(target))
        {
            return FALSE;
        }
        return SDL_SyncWindow(target);
    }
    BOOL WINAPI AdjustWindowRectEx(LPRECT rectangle, DWORD style, BOOL menu, DWORD extended_style)
    {
        if (!rectangle)
        {
            return FALSE;
        }
        if (menu)
        {
            unsupported("AdjustWindowRectEx", "native menus");
        }
        if (extended_style & ~DWORD(8))
        {
            unsupported("AdjustWindowRectEx", "extended window decorations");
        }
        if (!(style & 0x00c40000U))
        {
            return TRUE;
        }
        if (!current_window)
        {
            unsupported("AdjustWindowRectEx", "no host window for decoration metrics");
        }
        SDL_Window* target = window(current_window, "AdjustWindowRectEx");
        Borders frame;
        if (!target || !borders(target, frame))
        {
            return FALSE;
        }
        const std::int64_t l = std::int64_t(rectangle->left) - frame.left,
                           t = std::int64_t(rectangle->top) - frame.top,
                           r = std::int64_t(rectangle->right) + frame.right,
                           b = std::int64_t(rectangle->bottom) + frame.bottom;
        if (!coordinate(l) || !coordinate(t) || !coordinate(r) || !coordinate(b))
        {
            return FALSE;
        }
        return SetRect(rectangle, l, t, r, b);
    }
    BOOL WINAPI ShowWindow(HWND handle, int command)
    {
        SDL_Window* target = window(handle, "ShowWindow");
        if (!target)
        {
            return FALSE;
        }
        const bool visible = !(SDL_GetWindowFlags(target) & SDL_WINDOW_HIDDEN);
        if (keep_hidden)
        {
            return visible;
        }
#if defined(TARGET_OS_IOS) && TARGET_OS_IOS
        // An iOS window always fills its scene; it has no restored or minimized state.
        SDL_ShowWindow(target);
        return visible;
#endif
        bool ok = false;
        switch (command)
        {
        case 0:
            ok = SDL_HideWindow(target);
            break;
        case 2:
        case 6:
        case 7:
        case 11:
            ok = SDL_MinimizeWindow(target);
            break;
        case 3:
            ok = SDL_MaximizeWindow(target);
            break;
        case 1:
        case 4:
        case 9:
            ok = SDL_RestoreWindow(target) && SDL_ShowWindow(target);
            break;
        case 5:
        case 8:
        case 10:
            ok = SDL_ShowWindow(target);
            break;
        default:
            unsupported("ShowWindow", "unknown show command");
        }
        if (!ok || !SDL_SyncWindow(target))
        {
            unsupported("ShowWindow", SDL_GetError());
        }
        return visible;
    }
    BOOL WINAPI UpdateWindow(HWND handle)
    {
        if (!window(handle, "UpdateWindow"))
        {
            return FALSE;
        }
        // GPU windows have no GDI update region. Video owns frame presentation.
        return TRUE;
    }
    HMENU WINAPI GetMenu(HWND handle)
    {
        window(handle, "GetMenu");
        return nullptr; // Play's window has no menu.
    }
    LONG WINAPI GetWindowLongA(HWND handle, int index)
    {
        SDL_Window* target = window(handle, "GetWindowLongA");
        if (!target)
        {
            return 0;
        }
        const auto flags = SDL_GetWindowFlags(target);
        if (index == -16)
        {
            DWORD style = flags & SDL_WINDOW_BORDERLESS ? 0x80000000U : 0x00c00000U;
            if (flags & SDL_WINDOW_RESIZABLE)
            {
                style |= 0x00040000U;
            }
            if (!(flags & SDL_WINDOW_HIDDEN))
            {
                style |= 0x10000000U;
            }
            if (flags & SDL_WINDOW_MINIMIZED)
            {
                style |= 0x20000000U;
            }
            if (flags & SDL_WINDOW_MAXIMIZED)
            {
                style |= 0x01000000U;
            }
            return std::bit_cast<LONG>(style);
        }
        if (index == -20)
        {
            return flags & SDL_WINDOW_ALWAYS_ON_TOP ? 8 : 0;
        }
        unsupported("GetWindowLongA", "only GWL_STYLE and GWL_EXSTYLE are available");
    }
    int WINAPI GetSystemMetrics(int index)
    {
        if (index == 0 || index == 1)
        {
            SDL_Rect bounds;
            if (!SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &bounds))
            {
                return 0;
            }
            return index ? bounds.h : bounds.w;
        }
        if (index == SM_CXSIZEFRAME || index == SM_CYSIZEFRAME || index == SM_CYCAPTION)
        {
            if (!current_window)
            {
                unsupported("GetSystemMetrics", "no host window for frame metrics");
            }
            Borders frame;
            SDL_Window* target = window(current_window, "GetSystemMetrics");
            if (!target || !borders(target, frame))
            {
                return 0;
            }
            if (index == SM_CXSIZEFRAME)
            {
                return frame.left;
            }
            if (index == SM_CYSIZEFRAME)
            {
                return frame.bottom;
            }
            return std::max(0, frame.top - frame.bottom);
        }
        unsupported("GetSystemMetrics", "unsupported system metric");
    }
    BOOL WINAPI SystemParametersInfoA(UINT action, UINT, PVOID value, UINT flags)
    {
        if (action != 0x30 || flags)
        {
            unsupported("SystemParametersInfoA", "only SPI_GETWORKAREA is available");
        }
        if (!value)
        {
            return FALSE;
        }
        SDL_Rect work;
        if (!SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &work))
        {
            return FALSE;
        }
        return SetRect(static_cast<RECT*>(value), work.x, work.y, work.x + work.w, work.y + work.h);
    }
    SHORT WINAPI GetAsyncKeyState(int key)
    {
        if (!SDL_IsMainThread())
        {
            unsupported("GetAsyncKeyState", "SDL keyboard state requires the main thread");
        }
        if (key < 1 || key > 255)
        {
            return 0;
        }
        start_events();
        SDL_PumpEvents();
        bool down = false;
        if (key == 1 || key == 2 || key == 4 || key == 5 || key == 6)
        {
            const Uint32 mask = key == 1   ? SDL_BUTTON_LMASK
                                : key == 2 ? SDL_BUTTON_RMASK
                                : key == 4 ? SDL_BUTTON_MMASK
                                : key == 5 ? SDL_BUTTON_X1MASK
                                           : SDL_BUTTON_X2MASK;
            down = SDL_GetMouseState(nullptr, nullptr) & mask;
        }
        else
        {
            int count;
            const bool* state = SDL_GetKeyboardState(&count);
            for (int scan = 0; scan < count; ++scan)
            {
                const int mapped = virtual_key(static_cast<SDL_Scancode>(scan));
                if (mapped == key || (key >= 0x10 && key <= 0x12 && mapped >= 0xa0 &&
                                         mapped <= 0xa5 && 0x10 + (mapped - 0xa0) / 2 == key))
                {
                    down |= state[scan];
                }
            }
        }
        if (!SDL_GetKeyboardFocus() && !SDL_GetMouseFocus())
        {
            down = false;
        }
        std::lock_guard lock(message_mutex);
        const bool edge = pressed[key];
        pressed[key] = false;
        return std::bit_cast<SHORT>(static_cast<WORD>((down ? 0x8000 : 0) | edge));
    }
    BOOL WINAPI GetCursorPos(LPPOINT point)
    {
        return point && beachlife::compat::input::cursor_position(*point);
    }
    BOOL WINAPI SetCursorPos(int x, int y)
    {
        return beachlife::compat::input::set_cursor_position(x, y);
    }
    BOOL WINAPI ClipCursor(const RECT* rectangle)
    {
        return beachlife::compat::input::clip_cursor(rectangle);
    }
    int WINAPI ShowCursor(BOOL show)
    {
        init_cursor();
        if (cursor_count ==
            (show ? std::numeric_limits<int>::max() : std::numeric_limits<int>::min()))
        {
            unsupported("ShowCursor", "display count overflow");
        }
        const int next = cursor_count + (show ? 1 : -1);
        const bool visible = next >= 0 && selected_cursor;
        if (!(visible ? SDL_ShowCursor() : SDL_HideCursor()))
        {
            unsupported("ShowCursor", SDL_GetError());
        }
        cursor_count = next;
        return cursor_count;
    }
    HCURSOR WINAPI SetCursor(HCURSOR cursor)
    {
        init_cursor();
        HCURSOR previous = selected_cursor;
        if (cursor && !SDL_SetCursor(reinterpret_cast<SDL_Cursor*>(cursor)))
        {
            return previous;
        }
        const bool visible = cursor && cursor_count >= 0;
        if (!(visible ? SDL_ShowCursor() : SDL_HideCursor()))
        {
            unsupported("SetCursor", SDL_GetError());
        }
        selected_cursor = cursor;
        return previous;
    }
    HANDLE WINAPI LoadImageA(
        HINSTANCE instance, LPCSTR name, UINT type, int width, int height, UINT flags)
    {
        if (!instance && reinterpret_cast<std::uintptr_t>(name) >= 32512 &&
            reinterpret_cast<std::uintptr_t>(name) <= 0xffff && type == 2 && width == 0 &&
            height == 0 && !(flags & ~UINT(0x8040)))
        {
            SDL_SystemCursor shape;
            switch (reinterpret_cast<std::uintptr_t>(name))
            {
            case 32512:
                shape = SDL_SYSTEM_CURSOR_DEFAULT;
                break;
            case 32513:
                shape = SDL_SYSTEM_CURSOR_TEXT;
                break;
            case 32514:
                shape = SDL_SYSTEM_CURSOR_WAIT;
                break;
            case 32515:
                shape = SDL_SYSTEM_CURSOR_CROSSHAIR;
                break;
            case 32642:
                shape = SDL_SYSTEM_CURSOR_NWSE_RESIZE;
                break;
            case 32643:
                shape = SDL_SYSTEM_CURSOR_NESW_RESIZE;
                break;
            case 32644:
                shape = SDL_SYSTEM_CURSOR_EW_RESIZE;
                break;
            case 32645:
                shape = SDL_SYSTEM_CURSOR_NS_RESIZE;
                break;
            case 32646:
                shape = SDL_SYSTEM_CURSOR_MOVE;
                break;
            case 32648:
                shape = SDL_SYSTEM_CURSOR_NOT_ALLOWED;
                break;
            case 32649:
                shape = SDL_SYSTEM_CURSOR_POINTER;
                break;
            case 32650:
                shape = SDL_SYSTEM_CURSOR_PROGRESS;
                break;
            default:
                unsupported("LoadImageA", "unknown predefined cursor");
            }
            auto& cursor = system_cursors[static_cast<unsigned>(shape)];
            if (!cursor)
            {
                cursor = SDL_CreateSystemCursor(shape);
            }
            return reinterpret_cast<HANDLE>(cursor);
        }
        return beachlife::compat::load_resource_cursor(instance, name, type, width, height, flags);
    }
    HICON WINAPI LoadIconA(HINSTANCE instance, LPCSTR name)
    {
        if (instance == GetModuleHandleA(nullptr) && reinterpret_cast<std::uintptr_t>(name) == 104)
        {
            // Play has already applied the bundled retail application icon.
            return reinterpret_cast<HICON>(&application_icon);
        }
        unsupported("LoadIconA", "only the application icon is available");
    }
    ATOM WINAPI RegisterClassExA(const WNDCLASSEXA* definition)
    {
        if (!definition || definition->cbSize != sizeof(WNDCLASSEXA) || !definition->lpfnWndProc ||
            reinterpret_cast<std::uintptr_t>(definition->lpszClassName) <= 0xffff)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return 0;
        }
        if (!class_name.empty())
        {
            SetLastError(1410); // ERROR_CLASS_ALREADY_EXISTS
            return 0;
        }
        if (definition->cbClsExtra || definition->cbWndExtra || definition->lpszMenuName)
        {
            unsupported("RegisterClassExA", "class storage and menus");
        }
        class_name = definition->lpszClassName;
        registered_class = *definition;
        registered_class.lpszClassName = class_name.c_str();
        return 1;
    }
    BOOL WINAPI UnregisterClassA(LPCSTR name, HINSTANCE instance)
    {
        const bool matches = reinterpret_cast<std::uintptr_t>(name) <= 0xffff
                                 ? reinterpret_cast<std::uintptr_t>(name) == 1
                                 : class_name == name;
        if (class_name.empty() || !matches || instance != registered_class.hInstance)
        {
            SetLastError(1411); // ERROR_CLASS_DOES_NOT_EXIST
            return FALSE;
        }
        if (window_alive)
        {
            SetLastError(1412); // ERROR_CLASS_HAS_WINDOWS
            return FALSE;
        }
        class_name.clear();
        registered_class = {};
        return TRUE;
    }
    HWND WINAPI CreateWindowExA(DWORD,
        LPCSTR name,
        LPCSTR title,
        DWORD,
        int,
        int,
        int,
        int,
        HWND parent,
        HMENU menu,
        HINSTANCE instance,
        LPVOID parameter)
    {
        const bool matches = reinterpret_cast<std::uintptr_t>(name) <= 0xffff
                                 ? reinterpret_cast<std::uintptr_t>(name) == 1
                                 : class_name == name;
        if (class_name.empty() || !matches || instance != registered_class.hInstance)
        {
            SetLastError(1407); // ERROR_CANNOT_FIND_WND_CLASS
            return nullptr;
        }
        if (!current_window || window_alive || parent || menu || parameter)
        {
            unsupported("CreateWindowExA", "only Play's top-level window is available");
        }
        // Play selected the host size/fullscreen mode before loading. Retail
        // geometry belongs to the renderer's logical 800x600 presentation.
        if (title && !SDL_SetWindowTitle(window(current_window, "CreateWindowExA"),
                         beachlife::compat::ansi_to_utf8(title).c_str()))
        {
            return nullptr;
        }
        beachlife::compat::bind_window_procedure(current_window, registered_class.lpfnWndProc);
        MSG activation{};
        activation.hwnd = current_window;
        activation.message = WM_ACTIVATEAPP;
        activation.wParam =
            SDL_GetKeyboardFocus() == beachlife::compat::video(current_window)->window() ||
            SDL_getenv("BEACHLIFE_BOOT_ASSUME_FOCUS");
        std::lock_guard lock(message_mutex);
        messages.push_back(activation);
        return current_window;
    }
    BOOL WINAPI DestroyWindow(HWND handle)
    {
        if (handle != current_window || !window_alive)
        {
            SetLastError(1400); // ERROR_INVALID_WINDOW_HANDLE
            return FALSE;
        }
        {
            std::lock_guard lock(message_mutex);
            window_alive = false;
            std::erase_if(messages,
                [handle](const MSG& message)
                {
                    return message.hwnd == handle;
                });
        }
        // Retire the Win32 window but retain Video for the retail loop's final
        // Update/Render and renderer teardown. Play releases SDL afterwards.
        window_procedure(handle, 2, 0, 0); // WM_DESTROY
        return TRUE;
    }
    BOOL WINAPI PeekMessageA(LPMSG message, HWND handle, UINT first, UINT last, UINT remove)
    {
        if (!SDL_IsMainThread())
        {
            unsupported("PeekMessageA", "SDL events require the main thread");
        }
        if (!message)
        {
            return FALSE;
        }
        if (remove & ~UINT(3))
        {
            unsupported("PeekMessageA", "unsupported queue flags");
        }
        if (handle)
        {
            window(handle, "PeekMessageA");
        }
        start_events();
        start_text_input();
        {
            std::lock_guard lock(message_mutex);
            queue_enabled = true;
        }
        if (prepare_frame)
        {
            prepare_frame(frame_context);
        }
        // Watchers feed both this queue and DirectInput. Drain SDL's copy so
        // it cannot fill up; return only one Win32 message to the retail loop.
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
        }
        std::lock_guard lock(message_mutex);
        for (auto it = messages.begin(); it != messages.end(); ++it)
        {
            if (it->message == WM_QUIT ||
                ((!handle || handle == it->hwnd) &&
                    ((!first && !last) || (it->message >= first && it->message <= last))))
            {
                *message = *it;
                if (remove & PM_REMOVE)
                {
                    messages.erase(it);
                }
                return TRUE;
            }
        }
        return FALSE;
    }
    BOOL WINAPI TranslateMessage(const MSG* message)
    {
        if (!message || (message->message != 0x100 && message->message != 0x101 &&
                            message->message != 0x104 && message->message != 0x105))
        {
            return FALSE;
        }
        if (!text_input_enabled)
        {
            unsupported("TranslateMessage", "no bound window for SDL keyboard-layout translation");
        }
        // SDL performs layout/dead-key translation while pumping events. The
        // watcher queues its text as ANSI WM_CHAR/WM_SYSCHAR messages.
        return TRUE;
    }
    LRESULT WINAPI DispatchMessageA(const MSG* message)
    {
        if (!message || !message->hwnd)
        {
            return 0;
        }
        if (message->hwnd != current_window || !window_procedure)
        {
            unsupported("DispatchMessageA", "attach the retail WNDPROC with bind_window_procedure");
        }
        try
        {
            return window_procedure(
                message->hwnd, message->message, message->wParam, message->lParam);
        }
        catch (...)
        {
            unsupported("DispatchMessageA", "window procedure threw an exception");
        }
    }
    void WINAPI PostQuitMessage(int exit_code)
    {
        MSG quit{};
        quit.message = WM_QUIT;
        quit.wParam = static_cast<WPARAM>(exit_code);
        std::lock_guard lock(message_mutex);
        messages.push_back(quit);
    }
    LRESULT WINAPI DefWindowProcA(HWND handle, UINT message, WPARAM parameter, LPARAM)
    {
        if (message == 0x10)
        {
            return DestroyWindow(handle);
        }
        if (message == 0x20)
        {
            SetCursor(reinterpret_cast<HCURSOR>(SDL_GetDefaultCursor()));
            return TRUE;
        }
        switch (message)
        {
        case 0:    // WM_NULL
        case 2:    // WM_DESTROY (the host still owns the Video)
        case 3:    // WM_MOVE
        case 5:    // WM_SIZE
        case 6:    // WM_ACTIVATE
        case 7:    // WM_SETFOCUS
        case 8:    // WM_KILLFOCUS
        case 0xf:  // WM_PAINT: the Video has no GDI update region
        case 0x1c: // WM_ACTIVATEAPP
        case 0x100:
        case 0x101:
        case 0x102:
        case 0x105:
        case 0x106: // WM_SYSCHAR: Play's window has no menu bar
            return 0;
        case 0x104: // WM_SYSKEYDOWN
            if (parameter == 0x73)
            {
                return window_procedure(handle, WM_CLOSE, 0, 0); // Alt-F4
            }
            return 0; // There is no host menu to activate with Alt/F10.
        default:
            unsupported("DefWindowProcA", "unimplemented default window message");
        }
    }
    int WINAPI MessageBoxA(HWND handle, LPCSTR text, LPCSTR caption, UINT type)
    {
        // The game's own messages, its fatal errors among them, also reach the game log.
        std::fprintf(
            stderr, "Beach Life game message: %s: %s\n", caption ? caption : "", text ? text : "");
        std::fflush(stderr);
        SDL_Window* target = handle ? window(handle, "MessageBoxA") : nullptr;
        if (type & ~UINT(0x3ff))
        {
            unsupported("MessageBoxA", "unsupported modality or message box options");
        }
        std::array<SDL_MessageBoxButtonData, 3> buttons{};
        int count = 0;
        const auto add = [&](int id, const char* label)
        {
            buttons[count++] = {0, id, label};
        };
        switch (type & 0xf)
        {
        case 0:
            add(1, "OK");
            break;
        case 1:
            add(1, "OK");
            add(2, "Cancel");
            break;
        case 2:
            add(3, "Abort");
            add(4, "Retry");
            add(5, "Ignore");
            break;
        case 3:
            add(6, "Yes");
            add(7, "No");
            add(2, "Cancel");
            break;
        case 4:
            add(6, "Yes");
            add(7, "No");
            break;
        case 5:
            add(4, "Retry");
            add(2, "Cancel");
            break;
        case 6:
            add(2, "Cancel");
            add(10, "Try Again");
            add(11, "Continue");
            break;
        default:
            unsupported("MessageBoxA", "unknown button set");
        }
        const unsigned default_button = (type >> 8) & 3;
        if (default_button >= static_cast<unsigned>(count))
        {
            return 0;
        }
        buttons[default_button].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
        for (int i = 0; i < count; ++i)
        {
            if (buttons[i].buttonID == 2 || count == 1)
            {
                buttons[i].flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
            }
        }
        SDL_MessageBoxFlags icon = 0;
        if ((type & 0xf0) == 0x10)
        {
            icon = SDL_MESSAGEBOX_ERROR;
        }
        else if ((type & 0xf0) == 0x30)
        {
            icon = SDL_MESSAGEBOX_WARNING;
        }
        else if (type & 0xf0)
        {
            icon = SDL_MESSAGEBOX_INFORMATION;
        }
        try
        {
            const auto title = beachlife::compat::ansi_to_utf8(caption ? caption : "");
            const auto body = beachlife::compat::ansi_to_utf8(text ? text : "");
            SDL_MessageBoxData data{
                icon, target, title.c_str(), body.c_str(), count, buttons.data(), nullptr};
            int result;
            return SDL_ShowMessageBox(&data, &result) ? result : 0;
        }
        catch (...)
        {
            return 0;
        }
    }
} // extern "C"
