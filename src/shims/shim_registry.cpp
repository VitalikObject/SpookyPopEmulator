#include "shim_registry.h"

#include "../emulator.h"
#include "../libc_abi.h"
#include "../libstdcpp_abi.h"
#include "modules.h"

#include <cstring>
#include <stdexcept>

namespace {

std::string_view StripLeadingUnderscore(const std::string_view name) {
    if (!name.empty() && name.front() == '_') {
        return name.substr(1);
    }
    return name;
}

bool StartsWith(const std::string_view text, const std::string_view prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::string DeriveClassName(const std::string_view symbol, const std::string_view prefix) {
    if (!StartsWith(symbol, prefix)) {
        throw std::runtime_error("symbol does not match expected Objective-C prefix: " + std::string(symbol));
    }
    return std::string(symbol.substr(prefix.size()));
}

}  // namespace

ShimContext::ShimContext(Emulator& emulator)
    : emulator_(emulator) {}

u32 ShimContext::RegisterFunction(const std::string& symbol, std::function<void(ShimContext&)> handler) {
    return emulator_.RegisterFunctionShim(symbol, [this, handler = std::move(handler)] {
        handler(*this);
    });
}

u32 ShimContext::RegisterData(const std::string& symbol, const std::span<const u8> bytes) {
    return emulator_.RegisterDataShim(symbol, bytes);
}

u32 ShimContext::RegisterStringConstant(const std::string& symbol, const std::string& value) {
    return emulator_.RegisterStringConstant(symbol, value);
}

u32 ShimContext::RegisterZeroData(const std::string& symbol, const std::size_t size) {
    std::vector<u8> zeros(size, 0);
    return RegisterData(symbol, zeros);
}

u32 ShimContext::BindAddress(const std::string& symbol, const u32 address) {
    if (const auto it = emulator_.import_cache_.find(symbol); it != emulator_.import_cache_.end()) {
        return it->second;
    }
    emulator_.import_cache_[symbol] = address;
    return address;
}

u32 ShimContext::RegisterClassImport(const std::string& symbol) {
    return BindAddress(symbol, EnsureClass(DeriveClassName(symbol, "_OBJC_CLASS_$_")));
}

u32 ShimContext::RegisterMetaClassImport(const std::string& symbol) {
    return BindAddress(symbol, EnsureMetaClass(DeriveClassName(symbol, "_OBJC_METACLASS_$_")));
}

u32 ShimContext::RegisterEhTypeImport(const std::string& symbol) {
    if (symbol == "_OBJC_EHTYPE_id") {
        return BindAddress(symbol, EnsureClass("NSObject"));
    }
    return BindAddress(symbol, EnsureClass(DeriveClassName(symbol, "_OBJC_EHTYPE_$_")));
}

u32 ShimContext::RegisterStdStreamCell(const std::string& symbol, const bool stderr_stream) {
    return emulator_.libc_abi_->RegisterStdStreamCell(symbol, stderr_stream);
}

u32 ShimContext::RegisterDispatchMainQueue(const std::string& symbol) {
    return emulator_.libc_abi_->RegisterDispatchMainQueue(symbol);
}

u32 ShimContext::RegisterStdStringEmptyRep(const std::string& symbol) {
    return emulator_.libstdcpp_abi_->RegisterEmptyRepStorage(symbol);
}

u32 ShimContext::RegisterCGAffineTransformIdentity(const std::string& symbol) {
    const std::array<float, 6> identity = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    std::array<u8, sizeof(identity)> bytes{};
    std::memcpy(bytes.data(), identity.data(), sizeof(identity));
    return RegisterData(symbol, bytes);
}

u32 ShimContext::RegisterCGRectZero(const std::string& symbol) {
    std::array<u8, 16> zeros{};
    return RegisterData(symbol, zeros);
}

u32 ShimContext::RegisterU32Data(const std::string& symbol, const u32 value) {
    std::array<u8, 4> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return RegisterData(symbol, bytes);
}

u32 ShimContext::RegisterF32Data(const std::string& symbol, const float value) {
    const u32 bits = std::bit_cast<u32>(value);
    return RegisterU32Data(symbol, bits);
}

u32 ShimContext::RegisterBooleanImport(const std::string& symbol, const bool value) {
    return BindAddress(symbol, EnsureBoolean(value));
}

u32 ShimContext::RegisterNumberImport(const std::string& symbol, const double value) {
    return BindAddress(symbol, EnsureNumber(value));
}

u32 ShimContext::CallGeneric(const std::string& symbol) {
    return emulator_.HandleGenericFunction(symbol);
}

u32 ShimContext::CallCoreFoundation(const std::string& symbol) {
    return emulator_.HandleCoreFoundationFunction(symbol);
}

u32 ShimContext::CallCppRuntime(const std::string& symbol) {
    return emulator_.HandleCppRuntimeFunction(symbol);
}

void ShimContext::SetReturnU32(const u32 value) {
    emulator_.SetReturnU32(value);
}

void ShimContext::SetReturnU64(const u64 value) {
    emulator_.SetReturnU64(value);
}

void ShimContext::SetReturnDouble(const double value) {
    emulator_.SetReturnDouble(value);
}

u32 ShimContext::EnsureClass(const std::string& class_name) {
    return emulator_.EnsureClass(class_name);
}

u32 ShimContext::EnsureMetaClass(const std::string& class_name) {
    return emulator_.EnsureMetaClass(class_name);
}

u32 ShimContext::EnsureNSString(const std::string& value) {
    return emulator_.EnsureNSString(value);
}

u32 ShimContext::EnsureNSData(const std::vector<u8>& data) {
    return emulator_.EnsureNSData(data);
}

u32 ShimContext::EnsureArray(const std::vector<u32>& values) {
    return emulator_.EnsureArray(values);
}

u32 ShimContext::EnsureDictionary(const std::unordered_map<std::string, u32>& values) {
    return emulator_.EnsureDictionary(values);
}

u32 ShimContext::EnsureNumber(const double value) {
    return emulator_.EnsureNumber(value);
}

u32 ShimContext::EnsureBoolean(const bool value) {
    return emulator_.EnsureBoolean(value);
}

u32 ShimContext::AllocateData(const u32 size, const u32 alignment, const std::string& tag) {
    return emulator_.AllocateData(size, alignment, tag);
}

u32 ShimContext::AllocateCString(const std::string& text, const std::string& tag) {
    return emulator_.AllocateCString(text, tag);
}

u32 ShimContext::Arg(const std::size_t index) const {
    return emulator_.Arg(index);
}

std::string ShimContext::ReadGuestCString(const u32 address) const {
    return emulator_.ReadGuestCString(address);
}

std::optional<std::string> ShimContext::DecodeNSString(const u32 address) const {
    return emulator_.DecodeNSString(address);
}

void ShimContext::Log(const std::string& line) const {
    emulator_.Log(line);
}

ShimRegistry::ShimRegistry(Emulator& emulator)
    : context_(emulator) {
    RegisterAllModules();
}

u32 ShimRegistry::ResolveImport(const std::string_view name, const int /*dylib_ordinal*/, const bool /*weak_import*/) {
    if (const auto it = resolvers_.find(std::string(name)); it != resolvers_.end()) {
        return it->second(context_);
    }

    const std::string symbol(name);
    if (StartsWith(symbol, "_OBJC_CLASS_$_")) {
        return context_.RegisterClassImport(symbol);
    }
    if (StartsWith(symbol, "_OBJC_METACLASS_$_")) {
        return context_.RegisterMetaClassImport(symbol);
    }
    if (StartsWith(symbol, "_OBJC_EHTYPE_")) {
        return context_.RegisterEhTypeImport(symbol);
    }
    if (symbol == "___CFConstantStringClassReference") {
        return context_.BindAddress(symbol, context_.EnsureClass("NSString"));
    }
    if (StartsWith(symbol, "__ZTV") || StartsWith(symbol, "__NSConcrete") || StartsWith(symbol, "__objc_empty_")) {
        return context_.RegisterZeroData(symbol, 64);
    }
    if (symbol == "__ZNSs4_Rep20_S_empty_rep_storageE") {
        return context_.RegisterStdStringEmptyRep(symbol);
    }
    if (StartsWith(symbol, "_k") || StartsWith(symbol, "_NS") || StartsWith(symbol, "_UI")
        || StartsWith(symbol, "_MPMovie") || StartsWith(symbol, "_GKErrorDomain")) {
        return context_.RegisterStringConstant(symbol, std::string(StripLeadingUnderscore(symbol)));
    }

    return context_.RegisterFunction(symbol, [symbol](ShimContext& ctx) {
        ctx.SetReturnU32(ctx.CallGeneric(symbol));
    });
}

void ShimRegistry::AddResolver(const std::string& symbol, Resolver resolver) {
    const auto [it, inserted] = resolvers_.emplace(symbol, std::move(resolver));
    if (!inserted) {
        throw std::runtime_error("duplicate shim registration for " + symbol);
    }
    (void)it;
}

void ShimRegistry::AddFunction(const std::string& symbol, RuntimeHandler handler) {
    AddResolver(symbol, [symbol, handler = std::move(handler)](ShimContext& ctx) {
        return ctx.RegisterFunction(symbol, handler);
    });
}

void ShimRegistry::AddGenericFunction(const std::string& symbol) {
    AddFunction(symbol, [symbol](ShimContext& ctx) {
        ctx.SetReturnU32(ctx.CallGeneric(symbol));
    });
}

void ShimRegistry::AddCoreFoundationFunction(const std::string& symbol) {
    AddFunction(symbol, [symbol](ShimContext& ctx) {
        ctx.SetReturnU32(ctx.CallCoreFoundation(symbol));
    });
}

void ShimRegistry::AddCppRuntimeFunction(const std::string& symbol) {
    AddFunction(symbol, [symbol](ShimContext& ctx) {
        ctx.SetReturnU32(ctx.CallCppRuntime(symbol));
    });
}

void ShimRegistry::AddStringConstant(const std::string& symbol, const std::string& value) {
    AddResolver(symbol, [symbol, value](ShimContext& ctx) {
        return ctx.RegisterStringConstant(symbol, value);
    });
}

void ShimRegistry::AddStringConstantBySymbol(const std::string& symbol) {
    AddStringConstant(symbol, std::string(StripLeadingUnderscore(symbol)));
}

void ShimRegistry::AddClassImport(const std::string& symbol) {
    AddResolver(symbol, [symbol](ShimContext& ctx) {
        return ctx.RegisterClassImport(symbol);
    });
}

void ShimRegistry::AddMetaClassImport(const std::string& symbol) {
    AddResolver(symbol, [symbol](ShimContext& ctx) {
        return ctx.RegisterMetaClassImport(symbol);
    });
}

void ShimRegistry::AddEhTypeImport(const std::string& symbol) {
    AddResolver(symbol, [symbol](ShimContext& ctx) {
        return ctx.RegisterEhTypeImport(symbol);
    });
}

void ShimRegistry::AddZeroData(const std::string& symbol, const std::size_t size) {
    AddResolver(symbol, [symbol, size](ShimContext& ctx) {
        return ctx.RegisterZeroData(symbol, size);
    });
}

void ShimRegistry::RegisterAllModules() {
    RegisterSelfShims(*this);
    RegisterCoreTelephonyShims(*this);
    RegisterSecurityShims(*this);
    RegisterAVFoundationShims(*this);
    RegisterFoundationShims(*this);
    RegisterUIKitShims(*this);
    RegisterOpenALShims(*this);
    RegisterOpenGLESShims(*this);
    RegisterAudioToolboxShims(*this);
    RegisterMediaPlayerShims(*this);
    RegisterStoreKitShims(*this);
    RegisterSystemConfigurationShims(*this);
    RegisterCoreGraphicsShims(*this);
    RegisterMessageUIShims(*this);
    RegisterQuartzCoreShims(*this);
    RegisterGameKitShims(*this);
    RegisterCoreFoundationShims(*this);
    RegisterLibStdCppShims(*this);
    RegisterLibSystemShims(*this);
    RegisterLibGccShims(*this);
    RegisterLibObjCShims(*this);
    RegisterLibSqlite3Shims(*this);
    RegisterSocialShims(*this);
    RegisterAccountsShims(*this);
    RegisterCoreMediaShims(*this);
}
