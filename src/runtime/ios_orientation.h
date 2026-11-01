#pragma once

#include <SDL3/SDL.h>

namespace beachlife::runtime
{
// iOS only: asks iPadOS for landscape if the window's scene is not in it.
void keep_landscape(SDL_Window* window);
// iOS only: 0 nominal, 1 fair, 2 serious, 3 critical.
int thermal_state();
} // namespace beachlife::runtime
