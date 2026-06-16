#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterStoreKitShims(ShimRegistry& registry) {
    static constexpr std::array kClasses{
        "_OBJC_CLASS_$_SKMutablePayment",
        "_OBJC_CLASS_$_SKPayment",
        "_OBJC_CLASS_$_SKPaymentQueue",
        "_OBJC_CLASS_$_SKProductsRequest",
        "_OBJC_CLASS_$_SKStoreProductViewController",
    };
    for (const char* symbol : kClasses) {
        registry.AddClassImport(symbol);
    }

    registry.AddStringConstantBySymbol("_SKStoreProductParameterITunesItemIdentifier");
}
