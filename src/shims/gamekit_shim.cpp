#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterGameKitShims(ShimRegistry& registry) {
    registry.AddStringConstantBySymbol("_GKErrorDomain");

    static constexpr std::array kClasses{
        "_OBJC_CLASS_$_GKAchievement",
        "_OBJC_CLASS_$_GKFriendRequestComposeViewController",
        "_OBJC_CLASS_$_GKLeaderboardViewController",
        "_OBJC_CLASS_$_GKLocalPlayer",
        "_OBJC_CLASS_$_GKPlayer",
        "_OBJC_CLASS_$_GKScore",
    };
    for (const char* symbol : kClasses) {
        registry.AddClassImport(symbol);
    }
}
