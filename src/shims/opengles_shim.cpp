#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterOpenGLESShims(ShimRegistry& registry) {
    registry.AddClassImport("_OBJC_CLASS_$_EAGLContext");

    static constexpr std::array kConstants{
        "_kEAGLColorFormatRGB565",
        "_kEAGLColorFormatRGBA8",
        "_kEAGLDrawablePropertyColorFormat",
        "_kEAGLDrawablePropertyRetainedBacking",
    };
    for (const char* symbol : kConstants) {
        registry.AddStringConstantBySymbol(symbol);
    }

    static constexpr std::array kFunctions{
        "_glActiveTexture",
        "_glAttachShader",
        "_glBindAttribLocation",
        "_glBindBuffer",
        "_glBindFramebuffer",
        "_glBindFramebufferOES",
        "_glBindRenderbuffer",
        "_glBindRenderbufferOES",
        "_glBindTexture",
        "_glBlendEquation",
        "_glBlendFunc",
        "_glBufferData",
        "_glCheckFramebufferStatus",
        "_glCheckFramebufferStatusOES",
        "_glClear",
        "_glClearColor",
        "_glCompileShader",
        "_glCompressedTexImage2D",
        "_glCreateProgram",
        "_glCreateShader",
        "_glDeleteBuffers",
        "_glDeleteFramebuffers",
        "_glDeleteFramebuffersOES",
        "_glDeleteProgram",
        "_glDeleteRenderbuffers",
        "_glDeleteRenderbuffersOES",
        "_glDeleteShader",
        "_glDeleteTextures",
        "_glDepthMask",
        "_glDisable",
        "_glDisableVertexAttribArray",
        "_glDiscardFramebufferEXT",
        "_glDrawArrays",
        "_glDrawElements",
        "_glEnable",
        "_glEnableVertexAttribArray",
        "_glFramebufferRenderbuffer",
        "_glFramebufferRenderbufferOES",
        "_glFramebufferTexture2D",
        "_glFramebufferTexture2DOES",
        "_glGenBuffers",
        "_glGenFramebuffers",
        "_glGenFramebuffersOES",
        "_glGenRenderbuffers",
        "_glGenRenderbuffersOES",
        "_glGenTextures",
        "_glGenerateMipmap",
        "_glGetError",
        "_glGetIntegerv",
        "_glGetProgramInfoLog",
        "_glGetProgramiv",
        "_glGetShaderInfoLog",
        "_glGetShaderiv",
        "_glGetString",
        "_glGetUniformLocation",
        "_glHint",
        "_glLinkProgram",
        "_glPixelStorei",
        "_glRenderbufferStorage",
        "_glRenderbufferStorageMultisampleAPPLE",
        "_glRenderbufferStorageOES",
        "_glResolveMultisampleFramebufferAPPLE",
        "_glScissor",
        "_glShaderSource",
        "_glTexImage2D",
        "_glTexParameterf",
        "_glTexParameteri",
        "_glTexSubImage2D",
        "_glUniform1i",
        "_glUniform2f",
        "_glUniform4f",
        "_glUniformMatrix4fv",
        "_glUseProgram",
        "_glVertexAttribPointer",
        "_glViewport",
    };
    for (const char* symbol : kFunctions) {
        registry.AddGenericFunction(symbol);
    }
}
