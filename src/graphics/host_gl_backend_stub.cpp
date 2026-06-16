#include "host_gl_backend.h"

#include <stdexcept>

namespace {

class StubHostGLBackend final : public HostGLBackend {
public:
    bool IsSupported() const override {
        return false;
    }

    void EnsureWindow(int, int, const std::string&) override {}
    void Resize(int, int) override {}
    void MakeCurrent() override {}
    void Present() override {}
    void PumpEvents() override {}

    void* GetProcAddress(const char*) override {
        return nullptr;
    }
};

}  // namespace

#if !defined(__APPLE__) && !defined(__ANDROID__)
std::unique_ptr<HostGLBackend> CreateHostGLBackend() {
    return std::make_unique<StubHostGLBackend>();
}
#endif
