#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace beachlife::runtime
{
// Turns raw fingers into the pointer actions the game understands. It knows nothing
// about SDL or the game, so every gesture can be tested with plain numbers.
//
//   One finger    tap: left click. Drag on the map: pan. While placing something: the
//                 preview follows, and a later tap places it. Painting paths and the
//                 interface: left drag. Hold still, then move: hover, no click.
//   Two fingers   drag: pan. Pinch: zoom. Tap: right click.
//   Three fingers tap: Escape.
// What a one-finger drag means where it starts.
enum class TouchDrag
{
    Button, // hold the left button: the interface, painting paths
    Pan,    // move the map
    Hover,  // move the cursor only: a press would place the building
};

struct TouchAction
{
    enum class Kind
    {
        Move,      // put the cursor at x, y
        LeftDown,  // at x, y
        LeftUp,    // at x, y
        RightDown, // at x, y
        RightUp,   // at x, y
        Wait,      // let the game see the previous action before the next one
        Pan,       // the content follows the fingers by x, y
        Zoom,      // steps: positive zooms in
        Escape,
    };
    Kind kind;
    double x = 0, y = 0;
    int steps = 0;
};

class TouchGestures
{
  public:
    static constexpr double slop = 10;         // points a finger may wander in a tap
    static constexpr double hold_seconds = 0.45;
    static constexpr double multi_tap_seconds = 0.35;
    static constexpr double zoom_ratio = 1.3; // finger spread per zoom level

    // Asked when a drag begins, with the cursor already at the drag's start.
    explicit TouchGestures(std::function<TouchDrag()> drag_under_cursor);

    void down(std::uint64_t finger, double x, double y, double seconds);
    void move(std::uint64_t finger, double x, double y, double seconds);
    void up(std::uint64_t finger, double x, double y, double seconds);
    // Fingers stolen by the system (a Home gesture, an alert).
    void cancel();
    // Call regularly: a hold is noticed while nothing moves.
    void tick(double seconds);

    std::vector<TouchAction> take();
    bool active() const
    {
        return !fingers_.empty();
    }

  private:
    struct Finger
    {
        std::uint64_t id;
        double x, y;
    };
    enum class State
    {
        Idle,
        Pending,  // one finger, not yet a tap, drag or hold
        Pan,      // one finger moving the map
        Drag,     // one finger holding the left button
        Hover,    // one finger moving the cursor, no button
        Multi,    // two fingers: pan, pinch or right click
        Three,    // three or more: Escape on release
        Finish,   // the gesture is decided; wait for every finger to lift
    };

    Finger* find(std::uint64_t finger);
    void remove(std::uint64_t finger);
    double spread() const;
    void centroid(double& x, double& y) const;
    void push(TouchAction::Kind kind, double x = 0, double y = 0, int steps = 0);
    void begin_multi(double seconds);
    void end_one_finger(double x, double y);

    std::function<TouchDrag()> drag_under_cursor_;
    std::vector<Finger> fingers_;
    std::vector<TouchAction> actions_;
    State state_ = State::Idle;
    double start_x_ = 0, start_y_ = 0, start_seconds_ = 0;
    double last_x_ = 0, last_y_ = 0;
    double zoom_spread_ = 0;
    bool multi_moved_ = false;
};
} // namespace beachlife::runtime
