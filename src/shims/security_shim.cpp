#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterSecurityShims(ShimRegistry& registry) {
    static constexpr std::array kConstants{
        "_kSecAttrAccessible",
        "_kSecAttrAccessibleWhenUnlocked",
        "_kSecAttrAccessibleWhenUnlockedThisDeviceOnly",
        "_kSecAttrAccount",
        "_kSecAttrApplicationTag",
        "_kSecAttrIsInvisible",
        "_kSecAttrKeyType",
        "_kSecAttrKeyTypeRSA",
        "_kSecAttrLabel",
        "_kSecAttrService",
        "_kSecClass",
        "_kSecClassGenericPassword",
        "_kSecClassKey",
        "_kSecReturnAttributes",
        "_kSecReturnData",
        "_kSecReturnRef",
        "_kSecValueData",
    };
    for (const char* symbol : kConstants) {
        registry.AddStringConstantBySymbol(symbol);
    }

    static constexpr std::array kFunctions{
        "_SecCertificateCreateWithData",
        "_SecItemAdd",
        "_SecItemCopyMatching",
        "_SecItemDelete",
        "_SecItemUpdate",
        "_SecKeyDecrypt",
        "_SecKeyEncrypt",
        "_SecKeyGetBlockSize",
        "_SecPolicyCreateBasicX509",
        "_SecTrustCopyPublicKey",
        "_SecTrustCreateWithCertificates",
        "_SecTrustEvaluate",
    };
    for (const char* symbol : kFunctions) {
        registry.AddCoreFoundationFunction(symbol);
    }
}
