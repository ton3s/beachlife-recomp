#include "runtime/touch_gestures.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using beachlife::runtime::TouchAction;
using beachlife::runtime::TouchGestures;
using Kind = TouchAction::Kind;

namespace
{
int failures = 0;

std::string names(const std::vector<TouchAction>& actions)
{
    std::string text;
    for (const auto& action : actions)
    {
        switch (action.kind)
        {
        case Kind::Move: text += "M"; break;
        case Kind::LeftDown: text += "D"; break;
        case Kind::LeftUp: text += "U"; break;
        case Kind::RightDown: text += "r"; break;
        case Kind::RightUp: text += "R"; break;
        case Kind::Wait: text += "."; break;
        case Kind::Pan: text += "P"; break;
        case Kind::Zoom: text += action.steps > 0 ? "+" : "-"; break;
        case Kind::Escape: text += "E"; break;
        }
    }
    return text;
}

void expect(const char* name, TouchGestures& gestures, const std::string& wanted)
{
    const auto got = names(gestures.take());
    if (got != wanted)
    {
        std::printf("%s: wanted %s, got %s\n", name, wanted.c_str(), got.c_str());
        ++failures;
    }
}
} // namespace

int main()
{
    bool on_map = true;
    bool placing = false;
    TouchGestures g(
        [&]
        {
            return placing  ? beachlife::runtime::TouchDrag::Hover
                   : on_map ? beachlife::runtime::TouchDrag::Pan
                            : beachlife::runtime::TouchDrag::Button;
        });

    placing = true;
    g.down(1, 100, 100, 0);
    g.move(1, 150, 100, 0.1);
    g.move(1, 180, 100, 0.2);
    g.up(1, 180, 100, 0.3);
    expect("placing: a drag only moves the preview", g, "MMM");
    g.down(1, 180, 100, 0.5);
    g.up(1, 180, 100, 0.6);
    expect("placing: the tap afterwards places", g, "MD.U");
    placing = false;

    g.down(1, 100, 100, 0);
    g.up(1, 102, 101, 0.1);
    expect("tap", g, "MD.U");

    g.down(1, 100, 100, 0);
    g.move(1, 150, 100, 0.1);
    g.move(1, 180, 100, 0.2);
    g.up(1, 180, 100, 0.3);
    expect("map drag pans, no click", g, "MPP");

    on_map = false;
    g.down(1, 100, 100, 0);
    g.move(1, 150, 100, 0.1);
    g.move(1, 180, 100, 0.2);
    g.up(1, 180, 100, 0.3);
    expect("interface drag holds the button", g, "MD.MMM.U");
    on_map = true;

    g.down(1, 100, 100, 0);
    g.tick(0.5);
    g.move(1, 150, 100, 0.6);
    g.up(1, 150, 100, 0.7);
    expect("hold hovers, never clicks", g, "MM");

    g.down(1, 100, 100, 0);
    g.tick(0.5);
    g.up(1, 100, 100, 0.6);
    expect("hold and lift does nothing", g, "M");

    g.down(1, 100, 100, 0);
    g.down(2, 200, 100, 0.01);
    g.up(1, 100, 100, 0.1);
    g.up(2, 200, 100, 0.11);
    expect("two-finger tap right clicks once", g, "MM.r.R");

    g.down(1, 100, 100, 0);
    g.down(2, 200, 100, 0.01);
    g.tick(1);
    g.up(1, 100, 100, 1);
    g.up(2, 200, 100, 1);
    expect("slow two-finger rest does nothing", g, "M");

    g.down(1, 100, 100, 0);
    g.down(2, 200, 100, 0.01);
    g.move(1, 100, 150, 0.1);
    g.move(2, 200, 150, 0.1);
    g.up(1, 100, 150, 0.2);
    g.up(2, 200, 150, 0.2);
    expect("two-finger drag pans", g, "MPP");

    g.down(1, 100, 100, 0);
    g.down(2, 200, 100, 0.01);
    g.move(2, 260, 100, 0.1);
    g.up(1, 100, 100, 0.2);
    g.up(2, 260, 100, 0.2);
    {
        const auto actions = g.take();
        std::string kinds = names(actions);
        if (kinds.find('+') == std::string::npos)
        {
            std::printf("pinch out should zoom in: %s\n", kinds.c_str());
            ++failures;
        }
    }

    g.down(1, 100, 100, 0);
    g.down(2, 300, 100, 0.01);
    g.move(2, 170, 100, 0.1);
    g.up(1, 100, 100, 0.2);
    g.up(2, 170, 100, 0.2);
    {
        const auto kinds = names(g.take());
        if (kinds.find('-') == std::string::npos)
        {
            std::printf("pinch in should zoom out: %s\n", kinds.c_str());
            ++failures;
        }
    }

    g.down(1, 100, 100, 0);
    g.down(2, 200, 100, 0.01);
    g.down(3, 300, 100, 0.02);
    g.up(1, 100, 100, 0.1);
    g.up(2, 200, 100, 0.1);
    g.up(3, 300, 100, 0.1);
    expect("three-finger tap is Escape", g, "ME");

    on_map = false;
    g.down(1, 100, 100, 0);
    g.move(1, 150, 100, 0.1);
    g.down(2, 200, 100, 0.2);
    g.up(1, 150, 100, 0.3);
    g.up(2, 200, 100, 0.3);
    {
        const auto kinds = names(g.take());
        if (kinds.find('U') == std::string::npos)
        {
            std::printf("a second finger must release the held button: %s\n", kinds.c_str());
            ++failures;
        }
    }

    g.down(1, 100, 100, 0);
    g.move(1, 150, 100, 0.1);
    g.cancel();
    {
        const auto kinds = names(g.take());
        if (kinds.find('U') == std::string::npos || g.active())
        {
            std::printf("cancel must release and forget: %s\n", kinds.c_str());
            ++failures;
        }
    }
    on_map = true;

    g.down(1, 100, 100, 0);
    g.up(1, 100, 100, 0.05);
    expect("next tap after cancel", g, "MD.U");

    if (!failures)
    {
        std::puts("touch gestures passed");
    }
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
