#pragma once

#include "common.h"
#include "guest_memory.h"
#include "macho_loader.h"

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/interface/halt_reason.h"

#include <deque>
#include <functional>
#include <initializer_list>

class ShimContext;
class ShimRegistry;
class HostGLBackend;
class LibcAbi;
class LibStdCppAbi;
class ObjcAbi;
struct RuntimeState;

enum class HostTouchPhase {
    Began,
    Moved,
    Ended,
    Cancelled,
};

struct HostTouchEvent {
    int pointer_id = 0;
    HostTouchPhase phase = HostTouchPhase::Began;
    float x_points = 0.0f;
    float y_points = 0.0f;
    double timestamp_seconds = 0.0;
};

enum class HostKeyPhase {
    Text,
    Backspace,
    Enter,
};

struct HostKeyEvent {
    HostKeyPhase phase = HostKeyPhase::Text;
    std::string text;
    double timestamp_seconds = 0.0;
};

using HostKeyboardVisibilityCallback = void (*)(bool visible);

struct HostPopupRequest {
    u32 token = 0;
    std::string class_name;
    std::string title;
    std::string message;
    std::vector<std::string> buttons;
    s32 cancel_button_index = -1;
    s32 preferred_button_index = -1;
};

struct HostPopupResult {
    u32 token = 0;
    s32 button_index = -1;
};

using HostPopupRequestCallback = void (*)(const HostPopupRequest& request);
using HostPopupDismissCallback = void (*)(u32 token);

struct EmulatorOptions {
    std::filesystem::path binary_path;
    std::optional<std::vector<u8>> binary_bytes;
    std::filesystem::path external_root;
    std::filesystem::path sandbox_root;
    std::function<bool(const std::string& path)> asset_exists;
    std::function<std::optional<std::vector<u8>>(const std::string& path)> read_asset;
    bool trace_shims = false;
};

class Emulator final : public Dynarmic::A32::UserCallbacks {
public:
    explicit Emulator(EmulatorOptions options);
    ~Emulator();

    int Run(bool run_initializers, bool run_main, std::uint64_t tick_budget);

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr vaddr) override;
    std::uint8_t MemoryRead8(Dynarmic::A32::VAddr vaddr) override;
    std::uint16_t MemoryRead16(Dynarmic::A32::VAddr vaddr) override;
    std::uint32_t MemoryRead32(Dynarmic::A32::VAddr vaddr) override;
    std::uint64_t MemoryRead64(Dynarmic::A32::VAddr vaddr) override;

    void MemoryWrite8(Dynarmic::A32::VAddr vaddr, std::uint8_t value) override;
    void MemoryWrite16(Dynarmic::A32::VAddr vaddr, std::uint16_t value) override;
    void MemoryWrite32(Dynarmic::A32::VAddr vaddr, std::uint32_t value) override;
    void MemoryWrite64(Dynarmic::A32::VAddr vaddr, std::uint64_t value) override;
    bool MemoryWriteExclusive8(Dynarmic::A32::VAddr vaddr, std::uint8_t value, std::uint8_t expected) override;
    bool MemoryWriteExclusive16(Dynarmic::A32::VAddr vaddr, std::uint16_t value, std::uint16_t expected) override;
    bool MemoryWriteExclusive32(Dynarmic::A32::VAddr vaddr, std::uint32_t value, std::uint32_t expected) override;
    bool MemoryWriteExclusive64(Dynarmic::A32::VAddr vaddr, std::uint64_t value, std::uint64_t expected) override;

    bool IsReadOnlyMemory(Dynarmic::A32::VAddr vaddr) override;
    void InterpreterFallback(Dynarmic::A32::VAddr pc, size_t num_instructions) override;
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(Dynarmic::A32::VAddr pc, Dynarmic::A32::Exception exception) override;
    void AddTicks(std::uint64_t ticks) override;
    std::uint64_t GetTicksRemaining() override;

private:
    friend class ShimContext;
    friend class ShimRegistry;
    friend class LibcAbi;
    friend class LibStdCppAbi;
    friend class ObjcAbi;

    enum class ObjKind {
        Generic,
        Class,
        MetaClass,
        String,
        Data,
        Array,
        Dictionary,
        Number,
        Boolean,
        FileHandle,
    };

    struct HostObject {
        ObjKind kind = ObjKind::Generic;
        std::string class_name;
        std::string string_value;
        std::vector<u8> bytes;
        std::vector<u32> items;
        std::unordered_map<std::string, u32> dict;
        std::unordered_map<u32, u32> properties_by_offset;
        double number_value = 0.0;
        bool boolean_value = false;
        u32 isa = 0;
        u32 meta = 0;
        u32 backing_store = 0;
    };

    struct FileHandle {
        std::FILE* file = nullptr;
        std::filesystem::path path;
        std::unique_ptr<std::vector<u8>> memory_bytes;
    };

    struct PendingUIApplicationMain {
        enum class Phase {
            Init,
            DidFinishLaunching,
            DidBecomeActive,
            Completed,
        };

        u32 original_lr = 0;
        u32 continuation_stub = 0;
        std::string principal_class;
        std::string delegate_class;
        u32 application = 0;
        u32 delegate = 0;
        u32 launch_options = 0;
        Phase phase = Phase::Init;
    };

    enum class ObjcCallbackResult {
        Completed,
        Yielded,
        Stopped,
    };

    struct PendingObjcCallback {
        u32 receiver = 0;
        std::string selector_name;
        std::vector<u32> args;
        u64 yield_count = 0;
        u32 last_pc = 0;
        u64 same_pc_yields = 0;
    };

    void BuildImage();
    void BuildCpu();
    void BuildProcessState();
    void CreateBuiltins();
    void InstallGuestFastPaths();

    u32 ResolveImport(std::string_view name, int dylib_ordinal, bool weak_import);
    u32 RegisterFunctionShim(const std::string& name, std::function<void()> handler);
    u32 RegisterFunctionShim(const std::string& name, std::function<void()> handler, bool tail_call);
    u32 RegisterDataShim(const std::string& name, std::span<const u8> bytes);
    u32 RegisterStringConstant(const std::string& name, const std::string& value);

    u32 AllocateGuest(u32& cursor, u32 size, u32 alignment, u8 permissions, const std::string& tag);
    u32 AllocateData(u32 size, u32 alignment, const std::string& tag);
    u32 AllocateExecutable(u32 size, u32 alignment, const std::string& tag);
    u32 AllocateCString(const std::string& text, const std::string& tag);

    u32 EnsureClass(const std::string& class_name);
    u32 EnsureMetaClass(const std::string& class_name);
    u32 EnsureNSString(const std::string& value);
    u32 EnsureNSData(const std::vector<u8>& data);
    u32 EnsureArray(const std::vector<u32>& values);
    u32 EnsureDictionary(const std::unordered_map<std::string, u32>& values);
    u32 EnsureDictionaryOfClass(const std::unordered_map<std::string, u32>& values, const std::string& class_name);
    u32 EnsureNumber(double value);
    u32 EnsureBoolean(bool value);
    u32 EnsureDate(double unix_seconds);
    u32 EnsureNSError(const std::string& domain, s32 code, u32 user_info);
    std::optional<std::string> DecodeNSString(u32 address) const;
    std::string DescribeNSObject(u32 address) const;
    std::string FormatVarArgsString(const std::string& format, std::size_t first_arg, const std::function<u32(std::size_t)>& read_arg) const;
    u32 ArchiveObject(u32 object);
    u32 UnarchiveObject(u32 data_object);
    void EnsureGuestAnnotation(u32 receiver);
    bool TryHandleObjcOverride(u32 receiver, const std::string& selector_name, u32* result);
    bool BeginObjcHostDispatch(u32 receiver, const std::string& selector_name, std::initializer_list<u32> args, u32 continuation_stub, u32* sync_result);
    u32 BeginUIApplicationMain(const std::string& principal_class, const std::string& delegate_class);
    void ContinueUIApplicationMain();
    void RunUIApplicationLoop();
    bool FireDisplayLinks();
    ObjcCallbackResult RunObjcCallback(u32 receiver, const std::string& selector_name, std::span<const u32> args);
    ObjcCallbackResult ResumeObjcCallback();
    void ProcessHostTouchEvents();
    void ProcessHostKeyEvents();
    void ProcessHostPopupResults();
    bool ApplyGuestForcedBoolReturn(u32 address, bool value, const std::string& symbol_name);
    void ApplyGuestLogicVersionPatches();
    void ApplyGuestLogicDefinesPatches();
    void SuppressGuestIapWarning(const HostObject& popup, s32 button_index);
    bool ApplyGuestIapWarningSuppression(const std::string& reason);
    std::filesystem::path IapWarningStatePath() const;
    bool HasPersistedIapWarningSuppression() const;
    void PersistIapWarningSuppression() const;
    void ObserveObjcKeyboardMessage(u32 receiver, const std::string& selector_name);
    bool BeginTickSlice();
    void RecordHotPcSample();
    void DumpHotPcSamples(std::size_t limit = 16) const;

    u32 DispatchObjCMessage(u32 self, u32 selector, bool is_super, bool stret, u32 stret_buffer);
    u32 HandleGenericFunction(const std::string& name);
    u32 HandleCoreFoundationFunction(const std::string& name);
    u32 HandlePosixFunction(const std::string& name);
    u32 HandleMathFunction(const std::string& name);
    u32 HandleGraphicsFunction(const std::string& name);
    u32 HandleSecurityFunction(const std::string& name);
    u32 HandleCppRuntimeFunction(const std::string& name);
    void HandlePrintfLike(const std::string& name);
    int HandleScanfLike(const std::string& name);

    u32 Arg(std::size_t index) const;
    void SetReturnU32(u32 value);
    void SetReturnU64(u64 value);
    void SetReturnDouble(double value);
    std::string ReadGuestCString(u32 address) const;
    std::filesystem::path ResolveGuestPath(const std::string& guest_path) const;
    std::optional<std::string> ResolveGuestAssetPath(const std::string& guest_path) const;
    std::optional<std::vector<u8>> ReadGuestFileBytes(const std::string& guest_path) const;
    bool GuestPathExists(const std::string& guest_path) const;
    void Stop(Dynarmic::HaltReason reason);
    void Log(const std::string& line) const;
    void SetErrno(int value);

    std::filesystem::path binary_path_;
    std::filesystem::path external_root_;
    std::filesystem::path sandbox_root_;
    std::function<bool(const std::string& path)> asset_exists_;
    std::function<std::optional<std::vector<u8>>(const std::string& path)> read_asset_;
    std::string guest_home_;
    std::string guest_tmp_;

    bool trace_shims_ = false;
    int exit_code_ = 0;
    std::uint64_t ticks_left_ = 0;
    std::uint64_t total_ticks_left_ = 0;
    bool returned_from_guest_ = false;
    bool saw_exit_ = false;
    std::string last_guest_error_;
    bool suppress_next_unwind_resume_ = false;

    MachOImage image_;
    GuestMemory memory_;
    std::unique_ptr<Dynarmic::ExclusiveMonitor> exclusive_monitor_;
    std::unique_ptr<Dynarmic::A32::Jit> cpu_;

    u32 heap_cursor_ = 0x10000000;
    u32 object_cursor_ = 0x20000000;
    u32 executable_cursor_ = 0x30000000;
    u32 stack_base_ = 0x70000000;
    u32 stack_size_ = 0x00100000;
    u32 stack_pointer_ = 0;

    u32 return_stub_ = 0;
    u32 uiapplicationmain_loop_stub_ = 0;
    u32 errno_address_ = 0;

    u32 next_svc_ = 0x100;
    std::unordered_map<std::string, u32> import_cache_;
    std::unordered_map<u32, std::string> import_name_by_stub_;
    std::unordered_map<u32, std::function<void()>> svc_handlers_;
    std::unordered_map<u32, std::string> svc_names_;
    u32 next_internal_stub_id_ = 1;

    std::unordered_map<u32, HostObject> host_objects_;
    std::unordered_map<std::string, std::pair<u32, u32>> class_cache_;
    std::unordered_map<u32, FileHandle> file_handles_;
    std::unordered_map<u32, u32> heap_alloc_sizes_;
    std::unique_ptr<RuntimeState> runtime_;
    u32 next_gl_name_ = 1;
    u32 next_audio_name_ = 1;
    std::unique_ptr<LibcAbi> libc_abi_;
    std::unique_ptr<LibStdCppAbi> libstdcpp_abi_;
    std::unique_ptr<ObjcAbi> objc_abi_;
    std::optional<PendingUIApplicationMain> pending_uiapplication_main_;
    std::optional<PendingObjcCallback> active_objc_callback_;
    std::deque<PendingObjcCallback> queued_objc_callbacks_;
    u32 next_host_popup_token_ = 1;
    std::unordered_map<u32, u32> active_host_popups_;
    bool iap_warning_suppressed_ = false;
    bool restore_persisted_iap_warning_suppression_ = false;
    std::unordered_map<u32, u64> hot_pc_samples_;
    std::unique_ptr<ShimRegistry> shim_registry_;
};

void EnqueueHostTouchEvent(const HostTouchEvent& event);
void EnqueueHostKeyEvent(const HostKeyEvent& event);
void EnqueueHostPopupResult(const HostPopupResult& result);
void SetHostKeyboardVisibilityCallback(HostKeyboardVisibilityCallback callback);
void SetHostPopupCallbacks(HostPopupRequestCallback request_callback, HostPopupDismissCallback dismiss_callback);
