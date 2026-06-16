#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterAudioToolboxShims(ShimRegistry& registry) {
    static constexpr std::array kFunctions{
        "_AudioFileClose",
        "_AudioFileGetProperty",
        "_AudioFileGetPropertyInfo",
        "_AudioFileOpenURL",
        "_AudioFileReadPackets",
        "_AudioQueueAllocateBuffer",
        "_AudioQueueDispose",
        "_AudioQueueEnqueueBuffer",
        "_AudioQueueFreeBuffer",
        "_AudioQueueNewOutput",
        "_AudioQueuePause",
        "_AudioQueueSetParameter",
        "_AudioQueueSetProperty",
        "_AudioQueueStart",
        "_AudioQueueStop",
        "_AudioServicesPlaySystemSound",
        "_AudioSessionGetProperty",
        "_AudioSessionInitialize",
        "_AudioSessionSetActive",
        "_AudioSessionSetProperty",
        "_ExtAudioFileDispose",
        "_ExtAudioFileGetProperty",
        "_ExtAudioFileOpenURL",
        "_ExtAudioFileRead",
        "_ExtAudioFileSetProperty",
    };
    for (const char* symbol : kFunctions) {
        registry.AddGenericFunction(symbol);
    }
}
