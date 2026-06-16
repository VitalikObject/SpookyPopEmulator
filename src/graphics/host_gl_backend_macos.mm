#if defined(__APPLE__)

#include "host_gl_backend.h"

#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>
#import <OpenGL/OpenGL.h>

#include <cmath>
#include <cstdio>
#include <dlfcn.h>
#include <mach-o/dyld.h>

namespace {

class MacOSHostGLBackend final : public HostGLBackend {
public:
    MacOSHostGLBackend() {
        bundle_ = CFBundleGetBundleWithIdentifier(CFSTR("com.apple.opengl"));
        if (bundle_ != nullptr) {
            CFRetain(bundle_);
            CFBundleLoadExecutable(bundle_);
            return;
        }

        CFURLRef url = CFURLCreateWithFileSystemPath(
            kCFAllocatorDefault,
            CFSTR("/System/Library/Frameworks/OpenGL.framework"),
            kCFURLPOSIXPathStyle,
            true);
        if (url != nullptr) {
            bundle_ = CFBundleCreate(kCFAllocatorDefault, url);
            CFRelease(url);
            if (bundle_ != nullptr) {
                CFBundleLoadExecutable(bundle_);
            }
        }
    }

    ~MacOSHostGLBackend() override {
        if (context_ != nil) {
            [NSOpenGLContext clearCurrentContext];
            [context_ clearDrawable];
            [context_ release];
        }
        if (window_ != nil) {
            [window_ orderOut:nil];
            [window_ release];
        }
        if (pixel_format_ != nil) {
            [pixel_format_ release];
        }
        if (bundle_ != nullptr) {
            CFRelease(bundle_);
        }
    }

    bool IsSupported() const override {
        return true;
    }

    void EnsureWindow(const int width, const int height, const std::string& title) override {
        @autoreleasepool {
            EnsureApplication();
            const bool created_window = window_ == nil;
            if (created_window) {
                const NSRect frame = NSMakeRect(80.0, 80.0, static_cast<CGFloat>(width), static_cast<CGFloat>(height));
                window_ = [[NSWindow alloc] initWithContentRect:frame
                                                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                        backing:NSBackingStoreBuffered
                                                          defer:NO];
                [window_ setReleasedWhenClosed:NO];
                [window_ setTitle:[NSString stringWithUTF8String:title.c_str()]];
                [window_ makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
            }

            if (pixel_format_ == nil) {
                const NSOpenGLPixelFormatAttribute legacy_attributes[] = {
                    NSOpenGLPFAAccelerated,
                    NSOpenGLPFAAllowOfflineRenderers,
                    NSOpenGLPFADoubleBuffer,
                    NSOpenGLPFAColorSize, 24,
                    NSOpenGLPFAAlphaSize, 8,
                    NSOpenGLPFADepthSize, 24,
                    NSOpenGLPFAStencilSize, 8,
                    NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersionLegacy,
                    0
                };
                pixel_format_ = [[NSOpenGLPixelFormat alloc] initWithAttributes:legacy_attributes];
                if (pixel_format_ == nil) {
                    const NSOpenGLPixelFormatAttribute core_attributes[] = {
                        NSOpenGLPFAAccelerated,
                        NSOpenGLPFAAllowOfflineRenderers,
                        NSOpenGLPFADoubleBuffer,
                        NSOpenGLPFAColorSize, 24,
                        NSOpenGLPFAAlphaSize, 8,
                        NSOpenGLPFADepthSize, 24,
                        NSOpenGLPFAStencilSize, 8,
                        NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
                        0
                    };
                    pixel_format_ = [[NSOpenGLPixelFormat alloc] initWithAttributes:core_attributes];
                }
                if (pixel_format_ == nil) {
                    const NSOpenGLPixelFormatAttribute minimal_attributes[] = {
                        NSOpenGLPFAAllowOfflineRenderers,
                        NSOpenGLPFADoubleBuffer,
                        NSOpenGLPFAColorSize, 24,
                        NSOpenGLPFAAlphaSize, 8,
                        0
                    };
                    pixel_format_ = [[NSOpenGLPixelFormat alloc] initWithAttributes:minimal_attributes];
                }
                if (pixel_format_ == nil) {
                    const NSOpenGLPixelFormatAttribute bare_attributes[] = {
                        NSOpenGLPFAAllowOfflineRenderers,
                        0
                    };
                    pixel_format_ = [[NSOpenGLPixelFormat alloc] initWithAttributes:bare_attributes];
                }
            }

            if (context_ == nil && pixel_format_ != nil) {
                context_ = [[NSOpenGLContext alloc] initWithFormat:pixel_format_ shareContext:nil];
                if (context_ != nil) {
                    [context_ setView:[window_ contentView]];
                    GLint swap_interval = 1;
                    [context_ setValues:&swap_interval forParameter:NSOpenGLCPSwapInterval];
                }
            }

            Resize(width, height);
            MakeCurrent();
            if (!reported_context_) {
                reported_context_ = true;
                std::fprintf(stderr,
                    "[host-gl] window=%dx%d pixel_format=%d context=%d current=%d cgl=%p\n",
                    width,
                    height,
                    pixel_format_ != nil ? 1 : 0,
                    context_ != nil ? 1 : 0,
                    [NSOpenGLContext currentContext] == context_ ? 1 : 0,
                    reinterpret_cast<void*>(CGLGetCurrentContext()));
            }
        }
    }

    void Resize(const int width, const int height) override {
        @autoreleasepool {
            if (window_ == nil) {
                return;
            }
            const NSSize content_size = NSMakeSize(static_cast<CGFloat>(width), static_cast<CGFloat>(height));
            const NSSize current_size = [[window_ contentView] frame].size;
            if (std::abs(current_size.width - content_size.width) > 0.5
                || std::abs(current_size.height - content_size.height) > 0.5) {
                [window_ setContentSize:content_size];
                [context_ update];
            }
        }
    }

    void MakeCurrent() override {
        @autoreleasepool {
            if (context_ != nil) {
                [context_ makeCurrentContext];
                CGLSetCurrentContext([context_ CGLContextObj]);
            }
        }
    }

    void Present() override {
        @autoreleasepool {
            if (context_ != nil) {
                [context_ flushBuffer];
            }
        }
    }

    void PumpEvents() override {
        @autoreleasepool {
            if (NSApp == nil) {
                return;
            }
            for (;;) {
                NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                    untilDate:[NSDate dateWithTimeIntervalSinceNow:0.0]
                                                       inMode:NSDefaultRunLoopMode
                                                      dequeue:YES];
                if (event == nil) {
                    break;
                }
                [NSApp sendEvent:event];
            }
        }
    }

    void* GetProcAddress(const char* name) override {
        if (bundle_ != nullptr) {
            CFStringRef symbol = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
            if (symbol != nullptr) {
                void* proc = reinterpret_cast<void*>(CFBundleGetFunctionPointerForName(bundle_, symbol));
                CFRelease(symbol);
                if (proc != nullptr) {
                    return proc;
                }
            }
        }
        const std::string underscored = "_" + std::string(name);
        if (NSIsSymbolNameDefined(underscored.c_str())) {
            if (NSSymbol symbol = NSLookupAndBindSymbol(underscored.c_str()); symbol != nullptr) {
                if (void* proc = NSAddressOfSymbol(symbol); proc != nullptr) {
                    return proc;
                }
            }
        }
        return dlsym(RTLD_DEFAULT, name);
    }

private:
    static void EnsureApplication() {
        static bool initialized = false;
        if (initialized) {
            return;
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        initialized = true;
    }

    NSWindow* window_ = nil;
    NSOpenGLPixelFormat* pixel_format_ = nil;
    NSOpenGLContext* context_ = nil;
    CFBundleRef bundle_ = nullptr;
    bool reported_context_ = false;
};

}  // namespace

std::unique_ptr<HostGLBackend> CreateHostGLBackend() {
    return std::make_unique<MacOSHostGLBackend>();
}

#endif
