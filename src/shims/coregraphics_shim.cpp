#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterCoreGraphicsShims(ShimRegistry& registry) {
    registry.AddResolver("_CGAffineTransformIdentity", [](ShimContext& ctx) {
        return ctx.RegisterCGAffineTransformIdentity("_CGAffineTransformIdentity");
    });
    registry.AddResolver("_CGRectZero", [](ShimContext& ctx) {
        return ctx.RegisterCGRectZero("_CGRectZero");
    });
    // CGSizeZero: {0.0f, 0.0f} = 8 bytes of zeros
    registry.AddResolver("_CGSizeZero", [](ShimContext& ctx) {
        return ctx.RegisterZeroData("_CGSizeZero", 8);
    });

    static constexpr std::array kFunctions{
        "_CGAffineTransformConcat",
        "_CGAffineTransformMakeRotation",
        "_CGAffineTransformMakeScale",
        "_CGAffineTransformMakeTranslation",
        "_CGAffineTransformRotate",
        "_CGAffineTransformScale",
        "_CGBitmapContextCreate",
        "_CGBitmapContextCreateImage",
        "_CGColorCreate",
        "_CGColorGetColorSpace",
        "_CGColorGetComponents",
        "_CGColorRelease",
        "_CGColorSpaceCreateDeviceRGB",
        "_CGColorSpaceRelease",
        "_CGContextAddArc",
        "_CGContextAddArcToPoint",
        "_CGContextAddLineToPoint",
        "_CGContextAddPath",
        "_CGContextAddRect",
        "_CGContextBeginPath",
        "_CGContextBeginTransparencyLayer",
        "_CGContextClearRect",
        "_CGContextClip",
        "_CGContextClipToMask",
        "_CGContextClipToRect",
        "_CGContextClosePath",
        "_CGContextDrawImage",
        "_CGContextDrawLinearGradient",
        "_CGContextDrawPath",
        "_CGContextDrawRadialGradient",
        "_CGContextEndTransparencyLayer",
        "_CGContextFillEllipseInRect",
        "_CGContextFillPath",
        "_CGContextFillRect",
        "_CGContextMoveToPoint",
        "_CGContextRelease",
        "_CGContextRestoreGState",
        "_CGContextSaveGState",
        "_CGContextScaleCTM",
        "_CGContextSetAllowsAntialiasing",
        "_CGContextSetAllowsFontSubpixelPositioning",
        "_CGContextSetFillColor",
        "_CGContextSetFillColorWithColor",
        "_CGContextSetGrayFillColor",
        "_CGContextSetGrayStrokeColor",
        "_CGContextSetLineJoin",
        "_CGContextSetLineWidth",
        "_CGContextSetRGBFillColor",
        "_CGContextSetRGBStrokeColor",
        "_CGContextSetShadowWithColor",
        "_CGContextSetShouldSubpixelQuantizeFonts",
        "_CGContextSetStrokeColor",
        "_CGContextSetStrokeColorSpace",
        "_CGContextSetStrokeColorWithColor",
        "_CGContextSetTextMatrix",
        "_CGContextSetTextPosition",
        "_CGContextStrokeEllipseInRect",
        "_CGContextStrokeLineSegments",
        "_CGContextStrokePath",
        "_CGContextTranslateCTM",
        "_CGDataProviderCreateWithCFData",
        "_CGFontCreateWithDataProvider",
        "_CGGradientCreateWithColorComponents",
        "_CGGradientCreateWithColors",
        "_CGGradientRelease",
        "_CGImageCreateWithImageInRect",
        "_CGImageGetAlphaInfo",
        "_CGImageGetBitmapInfo",
        "_CGImageGetBitsPerComponent",
        "_CGImageGetColorSpace",
        "_CGImageGetHeight",
        "_CGImageGetWidth",
        "_CGImageRelease",
        "_CGPathAddRect",
        "_CGPathCreateMutable",
        "_CGRectContainsPoint",
        "_CGRectEqualToRect",
        "_CGRectGetHeight",
        "_CGRectGetMaxX",
        "_CGRectGetMaxY",
        "_CGRectGetMidX",
        "_CGRectGetMidY",
        "_CGRectGetMinX",
        "_CGRectGetMinY",
        "_CGRectGetWidth",
        "_CGRectInset",
        "_CGRectOffset",
    };
    for (const char* symbol : kFunctions) {
        registry.AddCoreFoundationFunction(symbol);
    }
}
