#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterLibObjCShims(ShimRegistry& registry) {
    registry.AddEhTypeImport("_OBJC_EHTYPE_id");
    registry.AddCppRuntimeFunction("___objc_personality_v0");
    registry.AddZeroData("__objc_empty_cache", 64);
    registry.AddZeroData("__objc_empty_vtable", 64);

    static constexpr std::array kFunctions{
        "_class_getMethodImplementation",
        "_objc_autorelease",
        "_objc_autoreleaseReturnValue",
        "_objc_begin_catch",
        "_objc_copyStruct",
        "_objc_copyWeak",
        "_objc_destroyWeak",
        "_objc_end_catch",
        "_objc_enumerationMutation",
        "_objc_exception_rethrow",
        "_objc_exception_throw",
        "_objc_getProperty",
        "_objc_initWeak",
        "_objc_loadWeakRetained",
        "_objc_msgSend",
        "_objc_msgSendSuper2",
        "_objc_msgSendSuper2_stret",
        "_objc_msgSend_stret",
        "_objc_release",
        "_objc_retain",
        "_objc_retainAutorelease",
        "_objc_retainAutoreleaseReturnValue",
        "_objc_retainAutoreleasedReturnValue",
        "_objc_retainBlock",
        "_objc_setAssociatedObject",
        "_objc_setProperty",
        "_objc_storeStrong",
        "_objc_storeWeak",
        "_objc_sync_enter",
        "_objc_sync_exit",
        "_protocol_conformsToProtocol",
        "_protocol_copyMethodDescriptionList",
        "_protocol_copyProtocolList",
    };
    for (const char* symbol : kFunctions) {
        registry.AddGenericFunction(symbol);
    }
}
