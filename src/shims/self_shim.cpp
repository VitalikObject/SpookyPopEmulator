#include "modules.h"
#include "shim_registry.h"

void RegisterSelfShims(ShimRegistry& registry) {
    (void)registry;
    // <self> symbols in this binary are local and do not participate in dyld import binding.
}
