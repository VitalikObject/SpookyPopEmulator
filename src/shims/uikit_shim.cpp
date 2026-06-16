#include "modules.h"
#include "shim_registry.h"

#include <array>
#include <limits>

void RegisterUIKitShims(ShimRegistry& registry) {
    static constexpr std::array kClasses{
        "_OBJC_CLASS_$_UIAccelerometer",
        "_OBJC_CLASS_$_UIActionSheet",
        "_OBJC_CLASS_$_UIActivityIndicatorView",
        "_OBJC_CLASS_$_UIAlertView",
        "_OBJC_CLASS_$_UIApplication",
        "_OBJC_CLASS_$_UIBarButtonItem",
        "_OBJC_CLASS_$_UIBezierPath",
        "_OBJC_CLASS_$_UIButton",
        "_OBJC_CLASS_$_UIColor",
        "_OBJC_CLASS_$_UIDevice",
        "_OBJC_CLASS_$_UIFont",
        "_OBJC_CLASS_$_UIImage",
        "_OBJC_CLASS_$_UIImageView",
        "_OBJC_CLASS_$_UILabel",
        "_OBJC_CLASS_$_UILocalNotification",
        "_OBJC_CLASS_$_UINavigationController",
        "_OBJC_CLASS_$_UIPasteboard",
        "_OBJC_CLASS_$_UIScreen",
        "_OBJC_CLASS_$_UIScrollView",
        "_OBJC_CLASS_$_UITabBarController",
        "_OBJC_CLASS_$_UIView",
        "_OBJC_CLASS_$_UIViewController",
        "_OBJC_CLASS_$_UIWebView",
        "_OBJC_CLASS_$_UIWindow",
    };
    for (const char* symbol : kClasses) {
        registry.AddClassImport(symbol);
    }

    static constexpr std::array kMetaclasses{
        "_OBJC_METACLASS_$_UIButton",
        "_OBJC_METACLASS_$_UIView",
        "_OBJC_METACLASS_$_UIViewController",
        "_OBJC_METACLASS_$_UIWebView",
    };
    for (const char* symbol : kMetaclasses) {
        registry.AddMetaClassImport(symbol);
    }

    static constexpr std::array kConstants{
        "_UIApplicationDidBecomeActiveNotification",
        "_UIApplicationDidEnterBackgroundNotification",
        "_UIApplicationLaunchOptionsRemoteNotificationKey",
        "_UIApplicationWillEnterForegroundNotification",
        "_UIApplicationWillResignActiveNotification",
        "_UIApplicationWillTerminateNotification",
        "_UIDeviceOrientationDidChangeNotification",
        "_UIKeyboardWillHideNotification",
        "_UIKeyboardWillShowNotification",
        "_UILocalNotificationDefaultSoundName",
    };
    for (const char* symbol : kConstants) {
        registry.AddStringConstantBySymbol(symbol);
    }

    registry.AddResolver("_UIBackgroundTaskInvalid", [](ShimContext& ctx) {
        return ctx.RegisterU32Data("_UIBackgroundTaskInvalid", std::numeric_limits<u32>::max());
    });
    registry.AddResolver("_UIWindowLevelAlert", [](ShimContext& ctx) {
        return ctx.RegisterF32Data("_UIWindowLevelAlert", 2000.0f);
    });

    static constexpr std::array kFunctions{
        "_NSStringFromCGRect",
        "_NSStringFromCGSize",
        "_UIApplicationMain",
        "_UIGraphicsGetCurrentContext",
        "_UIImagePNGRepresentation",
    };
    for (const char* symbol : kFunctions) {
        registry.AddGenericFunction(symbol);
    }
}
