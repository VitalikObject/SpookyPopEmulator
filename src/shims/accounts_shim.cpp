#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterAccountsShims(ShimRegistry& registry) {
    registry.AddClassImport("_OBJC_CLASS_$_ACAccountStore");

    static constexpr std::array kConstants{
        "_ACFacebookAppIdKey",
        "_ACFacebookAudienceEveryone",
        "_ACFacebookAudienceFriends",
        "_ACFacebookAudienceKey",
        "_ACFacebookAudienceOnlyMe",
        "_ACFacebookPermissionsKey",
    };
    for (const char* symbol : kConstants) {
        registry.AddStringConstantBySymbol(symbol);
    }
}
