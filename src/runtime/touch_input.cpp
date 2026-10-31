#include "runtime/touch_input.h"

#include "platform/video.h"
#include "runtime/touch_gestures.h"

#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include "runtime/ios_orientation.h"
#endif
#endif

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

namespace
{
bool touch_mode = false;
int (*game_drag_under_cursor)(void) = nullptr;
void (*game_pan)(float, float) = nullptr;
void (*game_fling)(float, float) = nullptr;
int (*game_autosave)(void) = nullptr;
}

extern "C" void beachlife_touch_register_autosave(int (*autosave)(void))
{
    game_autosave = autosave;
}

extern "C" void beachlife_touch_register(
    int (*drag_under_cursor)(void), void (*pan)(float, float), void (*fling)(float, float))
{
    game_drag_under_cursor = drag_under_cursor;
    game_pan = pan;
    game_fling = fling;
}

extern "C" int beachlife_touch_mode(void)
{
    return touch_mode;
}

namespace beachlife::runtime
{
namespace
{
constexpr int frames_per_wait = 3;

double now_seconds()
{
    return static_cast<double>(SDL_GetTicksNS()) / 1e9;
}

// Often enough to limit what a crash or a forced quit can lose.
double autosave_seconds()
{
    const char* text = SDL_getenv("BEACHLIFE_AUTOSAVE_SECONDS");
    return text ? SDL_atof(text) : 120.0;
}

struct Scripted
{
    unsigned long long frame;
    char kind;
    std::uint64_t finger;
    double x, y;
};

std::vector<Scripted> scripted_fingers()
{
    std::vector<Scripted> result;
    const char* text = SDL_getenv("BEACHLIFE_BOOT_TOUCH");
    while (text && *text)
    {
        Scripted entry{};
        unsigned long long finger = 0;
        int used = 0;
        if (std::sscanf(text, "%llu:%c,%llu,%lf,%lf%n", &entry.frame, &entry.kind, &finger,
                &entry.x, &entry.y, &used) != 5)
        {
            std::fprintf(stderr, "Invalid BEACHLIFE_BOOT_TOUCH entry: %s\n", text);
            break;
        }
        entry.finger = finger;
        result.push_back(entry);
        text += used;
        text += *text == ';';
    }
    return result;
}
} // namespace

struct TouchInput::State
{
    explicit State(platform::Video& video_)
        : video(video_)
        , gestures(
            []
            {
                const int kind = game_drag_under_cursor ? game_drag_under_cursor() : 0;
                return kind == 1   ? TouchDrag::Pan
                       : kind == 2 ? TouchDrag::Hover
                                   : TouchDrag::Button;
            })
    {
    }

    platform::Video& video;
    TouchGestures gestures;
    std::deque<TouchAction> queue;
    int waiting = 0;
    bool background = false;
    double glide_x = 0, glide_y = 0;
    int glide_age = 0;
    int escape_release = 0;
    bool autosave_wanted = false;
    double last_autosave = now_seconds();
    double autosave_every = autosave_seconds();
    bool logging = SDL_getenv("BEACHLIFE_TOUCH_LOG") != nullptr;
    unsigned long long current_frame = 0;
    std::vector<Scripted> script = scripted_fingers();
    std::string touch_mouse, mouse_touch;

    // Window points to game pixels: panning moves the picture by what the finger moved.
    double game_per_point() const
    {
        const auto transform = video.presentation_transform();
        const auto left = transform.to_window(0, 0), right = transform.to_window(1, 0);
        const double points = right.x - left.x;
        return points > 0 ? 1 / points : 1;
    }

    void button(Uint8 which, bool down, double x, double y)
    {
        SDL_Window* window = video.window();
        SDL_WarpMouseInWindow(window, static_cast<float>(x), static_cast<float>(y));
        SDL_Event event{};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.windowID = SDL_GetWindowID(window);
        event.button.button = which;
        event.button.down = down;
        event.button.clicks = 1;
        event.button.x = static_cast<float>(x);
        event.button.y = static_cast<float>(y);
        SDL_PushEvent(&event);
    }

    void escape(bool down)
    {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.windowID = SDL_GetWindowID(video.window());
        event.key.scancode = SDL_SCANCODE_ESCAPE;
        event.key.key = SDLK_ESCAPE;
        event.key.down = down;
        SDL_PushEvent(&event);
    }

    void run_actions()
    {
        for (auto& action : gestures.take())
        {
            queue.push_back(action);
        }
        if (escape_release > 0 && --escape_release == 0)
        {
            escape(false);
        }
        if (waiting > 0)
        {
            --waiting;
            return;
        }
        double pan_x = 0, pan_y = 0;
        while (!queue.empty() && !waiting)
        {
            const TouchAction action = queue.front();
            queue.pop_front();
            if (logging)
            {
                std::fprintf(stderr, "Touch: frame %llu kind %d %.1f %.1f %d\n", current_frame,
                    static_cast<int>(action.kind), action.x, action.y, action.steps);
                std::fflush(stderr);
            }
            switch (action.kind)
            {
            case TouchAction::Kind::Move:
                SDL_WarpMouseInWindow(video.window(), static_cast<float>(action.x),
                    static_cast<float>(action.y));
                break;
            case TouchAction::Kind::LeftDown:
                button(SDL_BUTTON_LEFT, true, action.x, action.y);
                break;
            case TouchAction::Kind::LeftUp:
                button(SDL_BUTTON_LEFT, false, action.x, action.y);
                break;
            case TouchAction::Kind::RightDown:
                button(SDL_BUTTON_RIGHT, true, action.x, action.y);
                break;
            case TouchAction::Kind::RightUp:
                button(SDL_BUTTON_RIGHT, false, action.x, action.y);
                break;
            case TouchAction::Kind::Wait:
                waiting = frames_per_wait;
                break;
            case TouchAction::Kind::Pan:
                pan_x += action.x;
                pan_y += action.y;
                break;
            case TouchAction::Kind::Zoom:
            {
                SDL_Event event{};
                event.type = SDL_EVENT_MOUSE_WHEEL;
                event.wheel.windowID = SDL_GetWindowID(video.window());
                event.wheel.y = static_cast<float>(action.steps);
                event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
                SDL_PushEvent(&event);
                break;
            }
            case TouchAction::Kind::Escape:
                // The game polls keys: a press and release in one frame would go unseen.
                escape(true);
                escape_release = frames_per_wait;
                break;
            }
        }
        const double scale = game_per_point();
        if ((pan_x != 0 || pan_y != 0) && game_pan)
        {
            // The map follows the fingers, so the view moves the other way.
            game_pan(static_cast<float>(-pan_x * scale), static_cast<float>(-pan_y * scale));
            if (logging)
            {
                std::fprintf(stderr, "Touch: frame %llu pan points %.1f %.1f game-per-point %.3f\n",
                    current_frame, pan_x, pan_y, scale);
                std::fflush(stderr);
            }
            glide_x = pan_x;
            glide_y = pan_y;
            glide_age = 0;
        }
        else
        {
            ++glide_age;
        }
        // Lifting a moving finger lets the map glide; a finger that paused does not.
        if (!gestures.active() && (glide_x != 0 || glide_y != 0))
        {
            if (glide_age <= 2 && game_fling)
            {
                game_fling(static_cast<float>(-glide_x * scale), static_cast<float>(-glide_y * scale));
            }
            glide_x = glide_y = 0;
        }
    }

    void finger(const SDL_TouchFingerEvent& event, Uint32 type)
    {
        int width = 0, height = 0;
        SDL_GetWindowSize(video.window(), &width, &height);
        const double x = event.x * width, y = event.y * height, seconds = now_seconds();
        if (type == SDL_EVENT_FINGER_DOWN)
        {
            gestures.down(event.fingerID, x, y, seconds);
        }
        else if (type == SDL_EVENT_FINGER_MOTION)
        {
            gestures.move(event.fingerID, x, y, seconds);
        }
        else
        {
            gestures.up(event.fingerID, x, y, seconds);
        }
    }

    static bool SDLCALL watch(void* context, SDL_Event* event)
    {
        auto& self = *static_cast<State*>(context);
        switch (event->type)
        {
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
            self.finger(event->tfinger, event->type);
            break;
        case SDL_EVENT_FINGER_CANCELED:
            self.gestures.cancel();
            break;
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            self.background = true;
            self.autosave_wanted = true;
            self.gestures.cancel();
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            self.background = false;
            break;
        default:
            break;
        }
        return true;
    }

    void autosave(const char* why)
    {
        autosave_wanted = false;
        last_autosave = now_seconds();
        if (!game_autosave)
        {
            return;
        }
        const double started = now_seconds();
        const int saved = game_autosave();
        if (saved)
        {
            std::fprintf(stderr, "Autosave (%s) took %.0f ms\n", why, (now_seconds() - started) * 1000);
            std::fflush(stderr);
        }
    }

    void run_script(unsigned long long frame_number)
    {
        const auto transform = video.presentation_transform();
        int width = 0, height = 0;
        SDL_GetWindowSize(video.window(), &width, &height);
        for (const auto& entry : script)
        {
            if (entry.frame != frame_number)
            {
                continue;
            }
            const auto point = transform.to_window(entry.x, entry.y);
            SDL_Event event{};
            event.type = entry.kind == 'd'   ? SDL_EVENT_FINGER_DOWN
                         : entry.kind == 'u' ? SDL_EVENT_FINGER_UP
                                             : SDL_EVENT_FINGER_MOTION;
            event.tfinger.fingerID = entry.finger;
            event.tfinger.x = static_cast<float>(point.x / width);
            event.tfinger.y = static_cast<float>(point.y / height);
            SDL_PushEvent(&event);
        }
    }
};

bool TouchInput::wanted()
{
#if defined(TARGET_OS_IOS) && TARGET_OS_IOS
    return true;
#else
    return SDL_getenv("BEACHLIFE_TOUCH") != nullptr;
#endif
}

TouchInput::TouchInput(platform::Video& video)
    : state_(std::make_unique<State>(video))
{
    touch_mode = true;
    const char* touch_mouse = SDL_GetHint(SDL_HINT_TOUCH_MOUSE_EVENTS);
    const char* mouse_touch = SDL_GetHint(SDL_HINT_MOUSE_TOUCH_EVENTS);
    state_->touch_mouse = touch_mouse ? touch_mouse : "1";
    state_->mouse_touch = mouse_touch ? mouse_touch : "1";
    // Fingers come only through the gestures. A real mouse or trackpad stays a mouse.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_AddEventWatch(State::watch, state_.get());
}

TouchInput::~TouchInput()
{
    SDL_RemoveEventWatch(State::watch, state_.get());
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, state_->touch_mouse.c_str());
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, state_->mouse_touch.c_str());
    touch_mode = false;
}

void TouchInput::frame(unsigned long long frame_number)
{
    // Between frames the world is consistent, and iOS still gives this moment to run.
    if (state_->autosave_wanted)
    {
        state_->autosave("leaving the app");
    }
    else if (state_->autosave_every > 0 &&
             now_seconds() - state_->last_autosave >= state_->autosave_every &&
             !state_->gestures.active() && state_->queue.empty())
    {
        state_->autosave("timer");
    }
    // iOS kills an app that draws in the background: wait here until it returns.
    while (state_->background)
    {
        SDL_PumpEvents();
        SDL_Delay(50);
    }
#if defined(TARGET_OS_IOS) && TARGET_OS_IOS
    if (frame_number % 30 == 0)
    {
        keep_landscape(state_->video.window());
    }
#endif
    state_->current_frame = frame_number;
    if (state_->logging && frame_number % 300 == 0)
    {
        std::fprintf(stderr, "Touch: frame %llu at %.1fs\n", frame_number, now_seconds());
        std::fflush(stderr);
    }
    state_->run_script(frame_number);
    state_->gestures.tick(now_seconds());
    state_->run_actions();
}
} // namespace beachlife::runtime
