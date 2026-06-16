#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterLibGccShims(ShimRegistry& registry) {
    static constexpr std::array kFunctions{
        "__Unwind_SjLj_Register",
        "__Unwind_SjLj_Resume",
        "__Unwind_SjLj_Unregister",
        "___divsi3",
        "___fixdfdi",
        "___fixunsdfdi",
        "___fixunssfdi",
        "___floatdidf",
        "___floatundidf",
        "___modsi3",
        "___udivdi3",
        "___udivsi3",
        "___umodsi3",
    };
    for (const char* symbol : kFunctions) {
        registry.AddCppRuntimeFunction(symbol);
    }
}
