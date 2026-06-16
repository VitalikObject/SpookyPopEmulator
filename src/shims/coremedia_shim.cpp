#include "modules.h"
#include "shim_registry.h"

void RegisterCoreMediaShims(ShimRegistry& registry) {
    // kCMTimeZero is a CMTime struct: {value=0, timescale=0, flags=0, epoch=0} = 32 bytes of zeros
    registry.AddZeroData("_kCMTimeZero", 32);

    registry.AddGenericFunction("_CMTimeMake");
}

