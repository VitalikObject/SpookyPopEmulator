#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterAVFoundationShims(ShimRegistry& registry) {
    static constexpr std::array kClasses{
        "_OBJC_CLASS_$_AVAudioPlayer",
        "_OBJC_CLASS_$_AVPlayer",
        "_OBJC_CLASS_$_AVPlayerLayer",
    };
    for (const char* symbol : kClasses) {
        registry.AddClassImport(symbol);
    }

    registry.AddStringConstantBySymbol("_AVPlayerItemDidPlayToEndTimeNotification");
}
