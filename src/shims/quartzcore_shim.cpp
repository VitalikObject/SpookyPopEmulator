#include "modules.h"
#include "shim_registry.h"

#include <array>
#include <cstring>

void RegisterQuartzCoreShims(ShimRegistry& registry) {
    // CATransform3DIdentity: 4x4 identity matrix = 64 bytes
    registry.AddResolver("_CATransform3DIdentity", [](ShimContext& ctx) {
        // 4x4 float identity matrix: zeros with 1.0f at [0], [5], [10], [15]
        float identity[16] = {};
        identity[0] = 1.0f;
        identity[5] = 1.0f;
        identity[10] = 1.0f;
        identity[15] = 1.0f;
        return ctx.RegisterData("_CATransform3DIdentity",
            std::span<const u8>(reinterpret_cast<const u8*>(identity), sizeof(identity)));
    });


    static constexpr std::array kClasses{
        "_OBJC_CLASS_$_CABasicAnimation",
        "_OBJC_CLASS_$_CAEAGLLayer",
        "_OBJC_CLASS_$_CAGradientLayer",
        "_OBJC_CLASS_$_CAKeyframeAnimation",
        "_OBJC_CLASS_$_CALayer",
        "_OBJC_CLASS_$_CAMediaTimingFunction",
        "_OBJC_CLASS_$_CATransaction",
    };
    for (const char* symbol : kClasses) {
        registry.AddClassImport(symbol);
    }

    registry.AddMetaClassImport("_OBJC_METACLASS_$_CABasicAnimation");

    static constexpr std::array kConstants{
        "_kCAFillModeBoth",
        "_kCAFillModeForwards",
        "_kCAMediaTimingFunctionEaseInEaseOut",
        "_kCATransactionDisableActions",
    };
    for (const char* symbol : kConstants) {
        registry.AddStringConstantBySymbol(symbol);
    }

    static constexpr std::array kFunctions{
        "_CATransform3DMakeRotation",
        "_CATransform3DMakeTranslation",
    };
    for (const char* symbol : kFunctions) {
        registry.AddGenericFunction(symbol);
    }
}
