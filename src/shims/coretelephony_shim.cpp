#include "modules.h"
#include "shim_registry.h"

void RegisterCoreTelephonyShims(ShimRegistry& registry) {
    registry.AddClassImport("_OBJC_CLASS_$_CTTelephonyNetworkInfo");
}
