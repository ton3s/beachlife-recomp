#include "runtime/ios_orientation.h"

#import <UIKit/UIKit.h>

namespace beachlife::runtime
{
// Info.plist and SDL's hint only say which orientations are allowed. iPadOS can still leave the
// scene in portrait, with the game letterboxed, so ask for landscape until it is granted.
void keep_landscape(SDL_Window* window)
{
    @autoreleasepool
    {
        UIWindow* ui_window = (__bridge UIWindow*)SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
        UIWindowScene* scene = ui_window.windowScene;
        if (!scene || UIInterfaceOrientationIsLandscape(scene.effectiveGeometry.interfaceOrientation))
        {
            return;
        }
        [ui_window.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
        UIWindowSceneGeometryPreferencesIOS* preferences = [[UIWindowSceneGeometryPreferencesIOS alloc]
            initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
        [scene requestGeometryUpdateWithPreferences:preferences
                                       errorHandler:^(NSError* error) {
                                           SDL_Log("Landscape request refused: %s",
                                               error.localizedDescription.UTF8String);
                                       }];
    }
}
} // namespace beachlife::runtime
