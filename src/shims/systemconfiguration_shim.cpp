#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterSystemConfigurationShims(ShimRegistry& registry) {
    static constexpr std::array kFunctions{
        "_SCNetworkReachabilityCreateWithAddress",
        "_SCNetworkReachabilityCreateWithName",
        "_SCNetworkReachabilityGetFlags",
        "_SCNetworkReachabilityScheduleWithRunLoop",
        "_SCNetworkReachabilitySetCallback",
        "_SCNetworkReachabilitySetDispatchQueue",
        "_SCNetworkReachabilityUnscheduleFromRunLoop",
    };
    for (const char* symbol : kFunctions) {
        registry.AddCoreFoundationFunction(symbol);
    }
}
