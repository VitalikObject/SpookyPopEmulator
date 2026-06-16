#include "modules.h"
#include "shim_registry.h"

#include <array>
#include <limits>

void RegisterCoreFoundationShims(ShimRegistry& registry) {
    static constexpr std::array kStringConstants{
        "_NSDefaultRunLoopMode",
        "_NSGregorianCalendar",
        "_NSInvalidArgumentException",
        "_NSLocaleCountryCode",
        "_NSLocaleCurrencyCode",
        "_NSLocaleCurrencySymbol",
        "_NSLocaleIdentifier",
        "_NSLocaleLanguageCode",
        "_NSRunLoopCommonModes",
        "_NSURLIsExcludedFromBackupKey",
        "_kCFBundleIdentifierKey",
        "_kCFBundleNameKey",
        "_kCFBundleVersionKey",
        "_kCFRunLoopCommonModes",
        "_kCFRunLoopDefaultMode",
        "_kCFStringTransformStripCombiningMarks",
        "_kCFStringTransformToLatin",
    };
    for (const char* symbol : kStringConstants) {
        registry.AddStringConstantBySymbol(symbol);
    }

    static constexpr std::array kClasses{
        "_OBJC_CLASS_$_NSArray",
        "_OBJC_CLASS_$_NSCache",
        "_OBJC_CLASS_$_NSCalendar",
        "_OBJC_CLASS_$_NSData",
        "_OBJC_CLASS_$_NSDate",
        "_OBJC_CLASS_$_NSDateComponents",
        "_OBJC_CLASS_$_NSDictionary",
        "_OBJC_CLASS_$_NSException",
        "_OBJC_CLASS_$_NSInputStream",
        "_OBJC_CLASS_$_NSInvocation",
        "_OBJC_CLASS_$_NSLocale",
        "_OBJC_CLASS_$_NSMutableArray",
        "_OBJC_CLASS_$_NSMutableData",
        "_OBJC_CLASS_$_NSMutableDictionary",
        "_OBJC_CLASS_$_NSMutableSet",
        "_OBJC_CLASS_$_NSNull",
        "_OBJC_CLASS_$_NSObject",
        "_OBJC_CLASS_$_NSOutputStream",
        "_OBJC_CLASS_$_NSRunLoop",
        "_OBJC_CLASS_$_NSSet",
        "_OBJC_CLASS_$_NSTimeZone",
        "_OBJC_CLASS_$_NSTimer",
        "_OBJC_CLASS_$_NSURL",
        "_OBJC_CLASS_$_NSUserDefaults",
    };
    for (const char* symbol : kClasses) {
        registry.AddClassImport(symbol);
    }

    registry.AddEhTypeImport("_OBJC_EHTYPE_$_NSException");
    registry.AddMetaClassImport("_OBJC_METACLASS_$_NSObject");
    registry.AddMetaClassImport("_OBJC_METACLASS_$_NSMutableArray");
    registry.AddMetaClassImport("_OBJC_METACLASS_$_NSMutableDictionary");

    registry.AddResolver("___CFConstantStringClassReference", [](ShimContext& ctx) {
        return ctx.BindAddress("___CFConstantStringClassReference", ctx.EnsureClass("NSString"));
    });
    registry.AddResolver("_kCFAllocatorDefault", [](ShimContext& ctx) {
        return ctx.RegisterZeroData("_kCFAllocatorDefault", 16);
    });
    registry.AddResolver("_kCFBooleanFalse", [](ShimContext& ctx) {
        return ctx.RegisterBooleanImport("_kCFBooleanFalse", false);
    });
    registry.AddResolver("_kCFBooleanTrue", [](ShimContext& ctx) {
        return ctx.RegisterBooleanImport("_kCFBooleanTrue", true);
    });
    registry.AddResolver("_kCFNumberNaN", [](ShimContext& ctx) {
        return ctx.RegisterNumberImport("_kCFNumberNaN", std::numeric_limits<double>::quiet_NaN());
    });
    registry.AddResolver("_kCFNumberNegativeInfinity", [](ShimContext& ctx) {
        return ctx.RegisterNumberImport("_kCFNumberNegativeInfinity", -std::numeric_limits<double>::infinity());
    });
    registry.AddResolver("_kCFNumberPositiveInfinity", [](ShimContext& ctx) {
        return ctx.RegisterNumberImport("_kCFNumberPositiveInfinity", std::numeric_limits<double>::infinity());
    });
    registry.AddResolver("_kCFTypeArrayCallBacks", [](ShimContext& ctx) {
        return ctx.RegisterZeroData("_kCFTypeArrayCallBacks", 32);
    });
    registry.AddResolver("_kCFTypeDictionaryKeyCallBacks", [](ShimContext& ctx) {
        return ctx.RegisterZeroData("_kCFTypeDictionaryKeyCallBacks", 32);
    });
    registry.AddResolver("_kCFTypeDictionaryValueCallBacks", [](ShimContext& ctx) {
        return ctx.RegisterZeroData("_kCFTypeDictionaryValueCallBacks", 32);
    });

    static constexpr std::array kFunctions{
        "_CFAbsoluteTimeGetCurrent",
        "_CFAllocatorCreate",
        "_CFArrayAppendValue",
        "_CFArrayCreate",
        "_CFArrayCreateMutable",
        "_CFArrayGetCount",
        "_CFArrayGetValueAtIndex",
        "_CFArrayRemoveAllValues",
        "_CFAttributedStringCreate",
        "_CFAttributedStringRemoveAttribute",
        "_CFAttributedStringSetAttribute",
        "_CFBitVectorCreate",
        "_CFBitVectorCreateMutableCopy",
        "_CFBitVectorGetBitAtIndex",
        "_CFBitVectorSetBitAtIndex",
        "_CFBooleanGetValue",
        "_CFBundleGetIdentifier",
        "_CFBundleGetMainBundle",
        "_CFBundleGetVersionNumber",
        "_CFDataCreate",
        "_CFDataCreateWithBytesNoCopy",
        "_CFDataGetBytePtr",
        "_CFDataGetLength",
        "_CFDictionaryCreate",
        "_CFDictionaryCreateMutable",
        "_CFDictionaryGetValue",
        "_CFDictionarySetValue",
        "_CFEqual",
        "_CFHash",
        "_CFLocaleCopyCurrent",
        "_CFLocaleGetSystem",
        "_CFReadStreamClose",
        "_CFReadStreamCreateWithFile",
        "_CFReadStreamOpen",
        "_CFReadStreamRead",
        "_CFRelease",
        "_CFRetain",
        "_CFRunLoopGetCurrent",
        "_CFStringAppendCharacters",
        "_CFStringAppendFormat",
        "_CFStringConvertNSStringEncodingToEncoding",
        "_CFStringCreateMutable",
        "_CFStringCreateWithCString",
        "_CFStringCreateWithSubstring",
        "_CFStringGetBytes",
        "_CFStringGetCStringPtr",
        "_CFStringGetCharactersPtr",
        "_CFStringGetFastestEncoding",
        "_CFStringGetLength",
        "_CFStringLowercase",
        "_CFStringTokenizerAdvanceToNextToken",
        "_CFStringTokenizerCopyBestStringLanguage",
        "_CFStringTokenizerCreate",
        "_CFStringTokenizerGetCurrentTokenRange",
        "_CFStringTransform",
        "_CFURLCreateFromFileSystemRepresentation",
        "_CFURLCreateStringByAddingPercentEscapes",
        "_CFURLCreateStringByReplacingPercentEscapesUsingEncoding",
        "_CFURLCreateWithFileSystemPath",
        "_CFUUIDCreate",
        "_CFUUIDCreateFromUUIDBytes",
        "_CFUUIDCreateString",
        "_CFUUIDGetUUIDBytes",
    };
    for (const char* symbol : kFunctions) {
        registry.AddCoreFoundationFunction(symbol);
    }
}
