#pragma once

#include <Helix/ShaderCompiler/Definition/ShaderDefinition.h>
#include <Helix/ShaderCompiler/Frontend/Frontend.h>

namespace helix::shadercompiler
{
    // Values of a shader feature keyword that materials use.
    struct KeywordSelection
    {
        StringView name;
        Span<const i64> values;
    };

    // The variants of one pass.
    struct VariantSet
    {
        // The keywords the code of the pass reads, in the order the shader declares them.
        Array<String> keywords;
        // One row per variant: a value for every keyword above.
        Array<Array<i64>> variants;

        // The keyword values of one variant in the form TranslateEntryPoint takes; the names point into this set.
        void Values(usize variant, Array<KeywordValue>& out) const;
        // Row of the variant that matches the given values (keywords not listed are off); -1 when there is none.
        i32 Find(Span<const KeywordValue> values) const;
    };

    // Lists every variant of a pass: all combinations of the values of the keywords its vertex and fragment shaders read.
    // Keywords the code never reads make no variants.
    //
    // A plain keyword takes all its values (false and true, or every member of its enum). A shader feature takes its
    // off value (false, or the first member) and the values in `used`, so variants no material asks for are not built.
    // Fails when a value in `used` is not a value of its keyword, when the code reads an enum keyword the shader does
    // not declare, or when there would be more than `limit` variants.
    bool EnumerateVariants(const Assembly& assembly, const ShaderDescription& shader, const ShaderPass& pass, Span<const KeywordSelection> used,
                           VariantSet& out, String& error, u32 limit = 4096);
}
