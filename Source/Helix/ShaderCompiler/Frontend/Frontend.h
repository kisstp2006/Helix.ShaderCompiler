#pragma once

#include <Helix/ShaderCompiler/Assembly/Assembly.h>
#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    // Value of one keyword of the variant to compile: a [Keyword] field is found by its name. Bool keywords are on
    // when the value is not zero, enum keywords take the value of the enum member.
    struct KeywordValue
    {
        StringView name;
        i64 value = 1;
    };

    struct TranslateOptions
    {
        // Keywords that are not listed are off (zero).
        Span<const KeywordValue> keywords;
    };

    // Translates a shader entry point (a static C# method) and every method it calls into a module.
    // `method` is a MethodDef row of the assembly. On failure `error` names the method and the reason.
    //
    // Supported code: float, int, uint, bool, the vector and matrix types, structs of those, static methods (also
    // generic ones), ref/out/in parameters, loops, conditionals, switch, and the functions of the shader library.
    //
    // Keyword fields are constants of the variant: the branches of other variants, and the functions and resources only
    // they use, are not part of the module. `out.keywords` lists the keywords the code read.
    bool TranslateEntryPoint(const Assembly& assembly, u32 method, Stage stage, Module& out, String& error, const TranslateOptions& options = {});
}
