#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterMediaPlayerShims(ShimRegistry& registry) {
    static constexpr std::array kConstants{
        "_MPMoviePlayerLoadStateDidChangeNotification",
        "_MPMoviePlayerPlaybackDidFinishNotification",
        "_MPMoviePlayerPlaybackDidFinishReasonUserInfoKey",
        "_MPMoviePlayerPlaybackStateDidChangeNotification",
        "_MPMoviePlayerWillEnterFullscreenNotification",
        "_MPMoviePlayerWillExitFullscreenNotification",
    };
    for (const char* symbol : kConstants) {
        registry.AddStringConstantBySymbol(symbol);
    }

    static constexpr std::array kClasses{
        "_OBJC_CLASS_$_MPMediaPickerController",
        "_OBJC_CLASS_$_MPMoviePlayerController",
        "_OBJC_CLASS_$_MPMoviePlayerViewController",
        "_OBJC_CLASS_$_MPMusicPlayerController",
    };
    for (const char* symbol : kClasses) {
        registry.AddClassImport(symbol);
    }

    registry.AddMetaClassImport("_OBJC_METACLASS_$_MPMoviePlayerViewController");
}
