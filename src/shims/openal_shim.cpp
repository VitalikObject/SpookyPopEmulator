#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterOpenALShims(ShimRegistry& registry) {
    static constexpr std::array kFunctions{
        "_alBufferData",
        "_alBufferDataStatic",
        "_alDeleteBuffers",
        "_alDeleteSources",
        "_alDistanceModel",
        "_alDopplerFactor",
        "_alDopplerVelocity",
        "_alGenBuffers",
        "_alGenSources",
        "_alGetError",
        "_alGetString",
        "_alGetSourcei",
        "_alIsExtensionPresent",
        "_alListener3f",
        "_alListenerf",
        "_alListenerfv",
        "_alSource3f",
        "_alSourcePause",
        "_alSourcePlay",
        "_alSourceQueueBuffers",
        "_alSourceRewind",
        "_alSourceStop",
        "_alSourceUnqueueBuffers",
        "_alSourcef",
        "_alSourcefv",
        "_alSourcei",
        "_alcCloseDevice",
        "_alcCreateContext",
        "_alcDestroyContext",
        "_alcGetContextsDevice",
        "_alcGetCurrentContext",
        "_alcGetError",
        "_alcGetIntegerv",
        "_alcGetProcAddress",
        "_alcGetString",
        "_alcIsExtensionPresent",
        "_alcMakeContextCurrent",
        "_alcOpenDevice",
        "_alcProcessContext",
        "_alcSuspendContext",
    };
    for (const char* symbol : kFunctions) {
        registry.AddGenericFunction(symbol);
    }
}
