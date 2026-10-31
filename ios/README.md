# iPad simulator build

Open `BeachLife.xcodeproj` (regenerate with `xcodegen --spec ios/project.yml`), pick an iPad
simulator and press Run. CMake builds the game (`build-game.sh`); Xcode installs and signs it.
Needs `brew install cmake ninja llvm` and `python3 tools/setup_dependencies.py`.
The scheme's `BEACHLIFE_IOS_ISO` names the ISO on this Mac and goes straight into the game.

## Touch controls

| Gesture | Action |
| --- | --- |
| Tap | Left click |
| One finger drag on the map | Pan, with a short glide on release |
| One finger drag while placing a building or scenery, or using the bulldozer | Moves the preview; tap to place |
| One finger drag on the interface, or painting paths | Left drag |
| Hold, then move | Hover: tooltips and placement preview, no click |
| Two finger drag | Pan |
| Pinch | Zoom (in placement, the game's wheel action rotates the building) |
| Two finger tap | Right click |
| Three finger tap | Escape |

A real mouse or trackpad stays a mouse. Edge scrolling is off in touch mode.
`src/runtime/touch_gestures.cpp` recognizes gestures and has unit tests;
`src/runtime/touch_input.cpp` feeds them to the game.
`BEACHLIFE_TOUCH=1` enables touch on desktop; `BEACHLIFE_BOOT_TOUCH` scripts fingers.

The app icon is a crop of `assets/launcher/header.jpg`; see `assets/launcher/README.md` for its provenance.
