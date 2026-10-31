#pragma once

#include <memory>

namespace beachlife::platform
{
class Video;
}

namespace beachlife::runtime
{
// Plays the game with fingers: on iOS, or anywhere with BEACHLIFE_TOUCH=1.
// BEACHLIFE_BOOT_TOUCH="frame:d|m|u,finger,x,y;..." scripts fingers in game coordinates.
class TouchInput
{
  public:
    explicit TouchInput(platform::Video& video);
    ~TouchInput();
    TouchInput(const TouchInput&) = delete;
    TouchInput& operator=(const TouchInput&) = delete;
    // Once per game frame, before the game reads its input.
    void frame(unsigned long long frame_number);
    static bool wanted();

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace beachlife::runtime

extern "C"
{
    // Whether fingers drive the game, read by the patched game source.
    int beachlife_touch_mode(void);
    // The game's touch tweak registers how to query and move its world. Builds without
    // the game leave these unset, and the gestures treat everything as interface.
    void beachlife_touch_register(
        int (*drag_under_cursor)(void), void (*pan)(float, float), void (*fling)(float, float));
}
