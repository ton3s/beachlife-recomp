# SDL patches

The pinned SDL revision remains unchanged. Prepared builds apply `series` to
its exported source; dependency checkouts are never edited.

`0001-guard-empty-display-modes.patch` skips the array copy when a display has
no fullscreen modes. SDL's dummy backend legitimately leaves the source pointer
null in that case. Calling `memcpy` with that pointer violates its nonnull
contract even for zero bytes and fails UBSan. The returned empty, null-terminated
mode list, count and all nonempty-mode behavior are preserved.

`0002-retain-xlib-process-cache.patch` retains the dynamically loaded base Xlib
module for the process lifetime. Xlib keeps locale and quark caches in module
globals; unloading the module loses those roots while the allocations remain.
Repeated SDL initialization reuses the retained handle. Optional X11 extension
modules still unload, and SDL still closes input methods, displays and windows
and resets its function pointers and module flags. This is qualified for the
statically linked SDL used by the application; unloading SDL itself is separate.

`0003-accept-ios-simulator-metal.patch` skips the GPU family check on the iOS
simulator, whose Metal device does not report `MTLGPUFamilyApple3`. Real devices
keep the check. It has no effect on other platforms.
