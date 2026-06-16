#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterSocialShims(ShimRegistry& registry) {
    registry.AddClassImport("_OBJC_CLASS_$_SLComposeViewController");

    static constexpr std::array kConstants{
        "_SLServiceTypeFacebook",
        "_SLServiceTypeSinaWeibo",
        "_SLServiceTypeTwitter",
    };
    for (const char* symbol : kConstants) {
        registry.AddStringConstantBySymbol(symbol);
    }
}
