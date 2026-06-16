#pragma once

#include "../common.h"

class Emulator;

class ShimContext final {
public:
    explicit ShimContext(Emulator& emulator);

    u32 RegisterFunction(const std::string& symbol, std::function<void(ShimContext&)> handler);
    u32 RegisterData(const std::string& symbol, std::span<const u8> bytes);
    u32 RegisterStringConstant(const std::string& symbol, const std::string& value);
    u32 RegisterZeroData(const std::string& symbol, std::size_t size);
    u32 BindAddress(const std::string& symbol, u32 address);

    u32 RegisterClassImport(const std::string& symbol);
    u32 RegisterMetaClassImport(const std::string& symbol);
    u32 RegisterEhTypeImport(const std::string& symbol);
    u32 RegisterStdStreamCell(const std::string& symbol, bool stderr_stream);
    u32 RegisterDispatchMainQueue(const std::string& symbol);
    u32 RegisterStdStringEmptyRep(const std::string& symbol);
    u32 RegisterCGAffineTransformIdentity(const std::string& symbol);
    u32 RegisterCGRectZero(const std::string& symbol);
    u32 RegisterU32Data(const std::string& symbol, u32 value);
    u32 RegisterF32Data(const std::string& symbol, float value);
    u32 RegisterBooleanImport(const std::string& symbol, bool value);
    u32 RegisterNumberImport(const std::string& symbol, double value);

    u32 CallGeneric(const std::string& symbol);
    u32 CallCoreFoundation(const std::string& symbol);
    u32 CallCppRuntime(const std::string& symbol);

    void SetReturnU32(u32 value);
    void SetReturnU64(u64 value);
    void SetReturnDouble(double value);

    u32 EnsureClass(const std::string& class_name);
    u32 EnsureMetaClass(const std::string& class_name);
    u32 EnsureNSString(const std::string& value);
    u32 EnsureNSData(const std::vector<u8>& data);
    u32 EnsureArray(const std::vector<u32>& values);
    u32 EnsureDictionary(const std::unordered_map<std::string, u32>& values);
    u32 EnsureNumber(double value);
    u32 EnsureBoolean(bool value);

    u32 AllocateData(u32 size, u32 alignment, const std::string& tag);
    u32 AllocateCString(const std::string& text, const std::string& tag);

    u32 Arg(std::size_t index) const;
    std::string ReadGuestCString(u32 address) const;
    std::optional<std::string> DecodeNSString(u32 address) const;
    void Log(const std::string& line) const;

private:
    Emulator& emulator_;
};
