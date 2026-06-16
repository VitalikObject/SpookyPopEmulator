#pragma once

#include "../common.h"
#include "shim_context.h"

class Emulator;

class ShimRegistry final {
public:
    using Resolver = std::function<u32(ShimContext&)>;
    using RuntimeHandler = std::function<void(ShimContext&)>;

    explicit ShimRegistry(Emulator& emulator);

    u32 ResolveImport(std::string_view name, int dylib_ordinal, bool weak_import);

    void AddResolver(const std::string& symbol, Resolver resolver);
    void AddFunction(const std::string& symbol, RuntimeHandler handler);
    void AddGenericFunction(const std::string& symbol);
    void AddCoreFoundationFunction(const std::string& symbol);
    void AddCppRuntimeFunction(const std::string& symbol);
    void AddStringConstant(const std::string& symbol, const std::string& value);
    void AddStringConstantBySymbol(const std::string& symbol);
    void AddClassImport(const std::string& symbol);
    void AddMetaClassImport(const std::string& symbol);
    void AddEhTypeImport(const std::string& symbol);
    void AddZeroData(const std::string& symbol, std::size_t size);

private:
    void RegisterAllModules();

    ShimContext context_;
    std::unordered_map<std::string, Resolver> resolvers_;
};
