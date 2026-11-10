#pragma once

#include <Helix/ShaderCompiler/Assembly/Assembly.h>
#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    // Translates a shader entry point (a static C# method) and every method it calls into a module.
    // `method` is a MethodDef row of the assembly. On failure `error` names the method and the reason.
    //
    // Supported code: float, int, uint, bool, the vector and matrix types, structs of those, static methods (also
    // generic ones), ref/out/in parameters, loops, conditionals, switch, and the functions of the shader library.
    bool TranslateEntryPoint(const Assembly& assembly, u32 method, Stage stage, Module& out, String& error);
}
