#include "runtime/touch_gestures.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace beachlife::runtime
{
using Kind = TouchAction::Kind;

TouchGestures::TouchGestures(std::function<TouchDrag()> drag_under_cursor)
    : drag_under_cursor_(std::move(drag_under_cursor))
{
}

TouchGestures::Finger* TouchGestures::find(std::uint64_t finger)
{
    for (auto& known : fingers_)
    {
        if (known.id == finger)
        {
            return &known;
        }
    }
    return nullptr;
}

void TouchGestures::remove(std::uint64_t finger)
{
    fingers_.erase(std::remove_if(fingers_.begin(), fingers_.end(),
                       [&](const Finger& known) { return known.id == finger; }),
        fingers_.end());
}

double TouchGestures::spread() const
{
    return fingers_.size() < 2
               ? 0
               : std::hypot(fingers_[0].x - fingers_[1].x, fingers_[0].y - fingers_[1].y);
}

void TouchGestures::centroid(double& x, double& y) const
{
    x = y = 0;
    for (const auto& finger : fingers_)
    {
        x += finger.x;
        y += finger.y;
    }
    x /= static_cast<double>(fingers_.size());
    y /= static_cast<double>(fingers_.size());
}

void TouchGestures::push(Kind kind, double x, double y, int steps)
{
    actions_.push_back({kind, x, y, steps});
}

// A button held when another finger lands must not stay down.
void TouchGestures::end_one_finger(double x, double y)
{
    if (state_ == State::Drag)
    {
        push(Kind::LeftUp, x, y);
    }
}

void TouchGestures::begin_multi(double seconds)
{
    state_ = State::Multi;
    start_seconds_ = seconds;
    multi_moved_ = false;
    centroid(last_x_, last_y_);
    start_x_ = last_x_;
    start_y_ = last_y_;
    zoom_spread_ = spread();
}

void TouchGestures::down(std::uint64_t finger, double x, double y, double seconds)
{
    if (find(finger))
    {
        return;
    }
    fingers_.push_back({finger, x, y});
    switch (fingers_.size())
    {
    case 1:
        state_ = State::Pending;
        start_x_ = last_x_ = x;
        start_y_ = last_y_ = y;
        start_seconds_ = seconds;
        push(Kind::Move, x, y);
        break;
    case 2:
        // After a one-finger gesture is decided, a late second finger still takes over:
        // resting a thumb while panning should not need lifting everything first.
        if (state_ != State::Finish)
        {
            end_one_finger(last_x_, last_y_);
            begin_multi(seconds);
        }
        break;
    default:
        if (state_ == State::Multi && !multi_moved_)
        {
            state_ = State::Three;
        }
        else
        {
            state_ = State::Finish;
        }
        break;
    }
}

void TouchGestures::move(std::uint64_t finger, double x, double y, double seconds)
{
    Finger* known = find(finger);
    if (!known)
    {
        return;
    }
    known->x = x;
    known->y = y;
    switch (state_)
    {
    case State::Pending:
        if (std::hypot(x - start_x_, y - start_y_) <= slop)
        {
            tick(seconds);
            break;
        }
        switch (drag_under_cursor_())
        {
        case TouchDrag::Pan:
            state_ = State::Pan;
            push(Kind::Pan, x - start_x_, y - start_y_);
            break;
        case TouchDrag::Hover:
            state_ = State::Hover;
            push(Kind::Move, x, y);
            break;
        case TouchDrag::Button:
            state_ = State::Drag;
            push(Kind::LeftDown, start_x_, start_y_);
            push(Kind::Wait);
            push(Kind::Move, x, y);
            break;
        }
        last_x_ = x;
        last_y_ = y;
        break;
    case State::Pan:
        push(Kind::Pan, x - last_x_, y - last_y_);
        last_x_ = x;
        last_y_ = y;
        break;
    case State::Drag:
    case State::Hover:
        push(Kind::Move, x, y);
        last_x_ = x;
        last_y_ = y;
        break;
    case State::Multi:
    {
        double cx = 0, cy = 0;
        centroid(cx, cy);
        const double now_spread = spread();
        if (!multi_moved_ && (std::hypot(cx - start_x_, cy - start_y_) > slop ||
                                 std::abs(now_spread - zoom_spread_) > slop))
        {
            multi_moved_ = true;
        }
        if (!multi_moved_)
        {
            break;
        }
        push(Kind::Pan, cx - last_x_, cy - last_y_);
        last_x_ = cx;
        last_y_ = cy;
        if (zoom_spread_ > 0 && now_spread > 0)
        {
            const double ratio = now_spread / zoom_spread_;
            if (ratio >= zoom_ratio)
            {
                push(Kind::Zoom, 0, 0, 1);
                zoom_spread_ = now_spread;
            }
            else if (ratio <= 1 / zoom_ratio)
            {
                push(Kind::Zoom, 0, 0, -1);
                zoom_spread_ = now_spread;
            }
        }
        break;
    }
    default:
        break;
    }
}

void TouchGestures::up(std::uint64_t finger, double x, double y, double seconds)
{
    Finger* known = find(finger);
    if (!known)
    {
        return;
    }
    known->x = x;
    known->y = y;
    switch (state_)
    {
    case State::Pending:
        push(Kind::LeftDown, start_x_, start_y_);
        push(Kind::Wait);
        push(Kind::LeftUp, start_x_, start_y_);
        state_ = State::Idle;
        break;
    case State::Drag:
        push(Kind::Move, x, y);
        push(Kind::Wait);
        push(Kind::LeftUp, x, y);
        state_ = State::Idle;
        break;
    case State::Pan:
    case State::Hover:
        state_ = State::Idle;
        break;
    case State::Multi:
        if (!multi_moved_ && seconds - start_seconds_ <= multi_tap_seconds)
        {
            push(Kind::Move, start_x_, start_y_);
            push(Kind::Wait);
            push(Kind::RightDown, start_x_, start_y_);
            push(Kind::Wait);
            push(Kind::RightUp, start_x_, start_y_);
        }
        state_ = State::Finish;
        break;
    case State::Three:
        if (seconds - start_seconds_ <= multi_tap_seconds)
        {
            push(Kind::Escape);
        }
        state_ = State::Finish;
        break;
    default:
        break;
    }
    remove(finger);
    if (fingers_.empty())
    {
        state_ = State::Idle;
    }
}

void TouchGestures::cancel()
{
    end_one_finger(last_x_, last_y_);
    fingers_.clear();
    state_ = State::Idle;
}

void TouchGestures::tick(double seconds)
{
    if (state_ == State::Pending && seconds - start_seconds_ >= hold_seconds)
    {
        state_ = State::Hover;
    }
}

std::vector<TouchAction> TouchGestures::take()
{
    return std::exchange(actions_, {});
}
} // namespace beachlife::runtime
