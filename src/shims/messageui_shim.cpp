#include "modules.h"
#include "shim_registry.h"

void RegisterMessageUIShims(ShimRegistry& registry) {
    registry.AddClassImport("_OBJC_CLASS_$_MFMailComposeViewController");
    registry.AddClassImport("_OBJC_CLASS_$_MFMessageComposeViewController");
    registry.AddMetaClassImport("_OBJC_METACLASS_$_MFMailComposeViewController");
}
