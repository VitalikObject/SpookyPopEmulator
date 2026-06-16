#pragma once

#include "../common.h"

#if defined(__ANDROID__)
struct ANativeWindow;
#endif

class HostGLBackend {
public:
    virtual ~HostGLBackend() = default;

    virtual bool IsSupported() const = 0;
    virtual void EnsureWindow(int width, int height, const std::string& title) = 0;
    virtual void Resize(int width, int height) = 0;
    virtual void MakeCurrent() = 0;
    virtual void Present() = 0;
    virtual void PumpEvents() = 0;
    virtual void* GetProcAddress(const char* name) = 0;
};

std::unique_ptr<HostGLBackend> CreateHostGLBackend();

#if defined(__ANDROID__)
void SetAndroidHostWindow(ANativeWindow* window);
#endif
