#pragma once

#include <SDL3/SDL.h>

namespace beachlife::runtime
{
// iOS only: asks iPadOS for landscape if the window's scene is not in it.
void keep_landscape(SDL_Window* window);
} // namespace beachlife::runtime
