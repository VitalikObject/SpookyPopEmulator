#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterFoundationShims(ShimRegistry& registry) {
    static constexpr std::array kConstants{
        "_NSFileCreationDate",
        "_NSFilePosixPermissions",
        "_NSFileSize",
        "_NSFileSystemFreeSize",
        "_NSFileSystemSize",
        "_NSKeyValueChangeNotificationIsPriorKey",
        "_NSLocalizedDescriptionKey",
        "_NSLocalizedFailureReasonErrorKey",
        "_NSMachErrorDomain",
        "_NSPOSIXErrorDomain",
        "_NSURLAuthenticationMethodServerTrust",
        "_NSURLErrorFailingURLStringErrorKey",
        "_NSUnderlyingErrorKey",
    };
    for (const char* symbol : kConstants) {
        registry.AddStringConstantBySymbol(symbol);
    }

    // NSFoundationVersionNumber: a double (~993.0 for iOS 7.x)
    registry.AddResolver("_NSFoundationVersionNumber", [](ShimContext& ctx) {
        // IEEE 754 double for 993.0
        double value = 993.0;
        return ctx.RegisterData("_NSFoundationVersionNumber",
            std::span<const u8>(reinterpret_cast<const u8*>(&value), sizeof(value)));
    });

    static constexpr std::array kClasses{
        "_OBJC_CLASS_$_NSAssertionHandler",
        "_OBJC_CLASS_$_NSAttributedString",
        "_OBJC_CLASS_$_NSAutoreleasePool",
        "_OBJC_CLASS_$_NSBundle",
        "_OBJC_CLASS_$_NSCharacterSet",
        "_OBJC_CLASS_$_NSDataDetector",
        "_OBJC_CLASS_$_NSDateFormatter",
        "_OBJC_CLASS_$_NSDecimalNumber",
        "_OBJC_CLASS_$_NSError",
        "_OBJC_CLASS_$_NSFileHandle",
        "_OBJC_CLASS_$_NSFileManager",
        "_OBJC_CLASS_$_NSHTTPCookieStorage",
        "_OBJC_CLASS_$_NSHTTPURLResponse",
        "_OBJC_CLASS_$_NSIndexPath",
        "_OBJC_CLASS_$_NSJSONSerialization",
        "_OBJC_CLASS_$_NSKeyedArchiver",
        "_OBJC_CLASS_$_NSKeyedUnarchiver",
        "_OBJC_CLASS_$_NSLayoutConstraint",
        "_OBJC_CLASS_$_NSLock",
        "_OBJC_CLASS_$_NSMutableAttributedString",
        "_OBJC_CLASS_$_NSMutableCharacterSet",
        "_OBJC_CLASS_$_NSMutableIndexSet",
        "_OBJC_CLASS_$_NSMutableString",
        "_OBJC_CLASS_$_NSMutableURLRequest",
        "_OBJC_CLASS_$_NSNotification",
        "_OBJC_CLASS_$_NSNotificationCenter",
        "_OBJC_CLASS_$_NSNumber",
        "_OBJC_CLASS_$_NSNumberFormatter",
        "_OBJC_CLASS_$_NSOperation",
        "_OBJC_CLASS_$_NSOperationQueue",
        "_OBJC_CLASS_$_NSPredicate",
        "_OBJC_CLASS_$_NSProcessInfo",
        "_OBJC_CLASS_$_NSPropertyListSerialization",
        "_OBJC_CLASS_$_NSRegularExpression",
        "_OBJC_CLASS_$_NSScanner",
        "_OBJC_CLASS_$_NSSortDescriptor",
        "_OBJC_CLASS_$_NSString",
        "_OBJC_CLASS_$_NSTextCheckingResult",
        "_OBJC_CLASS_$_NSThread",
        "_OBJC_CLASS_$_NSURLCache",
        "_OBJC_CLASS_$_NSURLConnection",
        "_OBJC_CLASS_$_NSURLCredential",
        "_OBJC_CLASS_$_NSURLRequest",
        "_OBJC_CLASS_$_NSURLResponse",
        "_OBJC_CLASS_$_NSUUID",
        "_OBJC_CLASS_$_NSValue",
        "_OBJC_CLASS_$_NSXMLParser",
    };
    for (const char* symbol : kClasses) {
        registry.AddClassImport(symbol);
    }

    static constexpr std::array kMetaclasses{
        "_OBJC_METACLASS_$_NSError",
        "_OBJC_METACLASS_$_NSOperation",
        "_OBJC_METACLASS_$_NSURLCache",
        "_OBJC_METACLASS_$_NSURLConnection",
    };
    for (const char* symbol : kMetaclasses) {
        registry.AddMetaClassImport(symbol);
    }

    static constexpr std::array kFunctions{
        "_NSClassFromString",
        "_NSGetUncaughtExceptionHandler",
        "_NSHomeDirectory",
        "_NSIntersectionRange",
        "_NSLog",
        "_NSLogv",
        "_NSSearchPathForDirectoriesInDomains",
        "_NSSelectorFromString",
        "_NSSetUncaughtExceptionHandler",
        "_NSStringFromClass",
        "_NSStringFromSelector",
        "_NSTemporaryDirectory",
        "__NSDictionaryOfVariableBindings",
    };
    for (const char* symbol : kFunctions) {
        registry.AddGenericFunction(symbol);
    }
}
