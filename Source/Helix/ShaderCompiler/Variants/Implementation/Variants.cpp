#include <Helix/ShaderCompiler/Variants/Variants.h>

#include <Helix/Core/Strings/Format.h>

namespace helix::shadercompiler
{
    void VariantSet::Values(usize variant, Array<KeywordValue>& out) const
    {
        out.Clear();
        for (usize k = 0; k < keywords.Size(); k++)
            out.Push({keywords[k], variants[variant][k]});
    }

    i32 VariantSet::Find(Span<const KeywordValue> values) const
    {
        for (usize v = 0; v < variants.Size(); v++)
        {
            bool match = true;
            for (usize k = 0; k < keywords.Size() && match; k++)
            {
                i64 wanted = 0;
                for (const KeywordValue& value : values)
                    if (value.name == keywords[k])
                        wanted = value.value;
                match = variants[v][k] == wanted;
            }
            if (match)
                return i32(v);
        }
        return -1;
    }

    bool EnumerateVariants(const Assembly& assembly, const ShaderDescription& shader, const ShaderPass& pass, Span<const KeywordSelection> used,
                           VariantSet& out, String& error, u32 limit)
    {
        out = VariantSet();
        error.Clear();
        // Translating with every keyword off reports the keywords the code reads, also those behind other keywords.
        Array<KeywordUse> read;
        const u32 methods[2] = {pass.vertex, pass.fragment};
        for (u32 stage = 0; stage < 2; stage++)
        {
            Module module;
            // A pass taken with UsePass keeps the code of its own assembly.
            const Assembly& code = pass.assembly ? *pass.assembly : assembly;
            if (!pass.usePass.Empty())
            {
                error = Format("shader {}: UsePass {} is not resolved; read the shaders with ReadShaderDefinitions or ResolveUsePasses", shader.name,
                               pass.usePass);
                return false;
            }
            if (!TranslateEntryPoint(code, methods[stage], stage == 0 ? Stage::Vertex : Stage::Fragment, module, error))
                return false;
            for (const KeywordUse& use : module.keywords)
            {
                // Dynamic keywords are uniforms and make no variants.
                if (use.dynamic)
                    continue;
                bool known = false;
                for (const KeywordUse& other : read)
                    known = known || other.name == use.name;
                if (!known)
                    read.Push(use);
            }
        }

        // The values each keyword can take, in the order of the shader's declarations, undeclared ones after them.
        Array<Array<i64>> choices;
        auto add = [&](StringView name, const ShaderKeyword* keyword) {
            Array<i64> values;
            if (!keyword || !keyword->shaderFeature)
            {
                if (keyword && !keyword->values.Empty())
                    values = keyword->values;
                else
                {
                    values.Push(0);
                    values.Push(1);
                }
            }
            else
            {
                values.Push(keyword->values.Empty() ? 0 : keyword->values[0]);
                for (const KeywordSelection& selection : used)
                {
                    if (selection.name != name)
                        continue;
                    for (i64 value : selection.values)
                    {
                        bool legal = keyword->values.Empty() && (value == 0 || value == 1);
                        for (i64 member : keyword->values)
                            legal = legal || member == value;
                        if (!legal)
                        {
                            error = Format("shader {}: {} is not a value of the keyword {}", shader.name, value, name);
                            return false;
                        }
                        if (!values.Find(value))
                            values.Push(value);
                    }
                }
            }
            out.keywords.Push(String(name));
            choices.Push(Move(values));
            return true;
        };
        for (const ShaderKeyword& keyword : shader.keywords)
            for (const KeywordUse& use : read)
                if (use.name == keyword.name && !add(keyword.name, &keyword))
                    return false;
        for (const KeywordUse& use : read)
        {
            if (shader.FindKeyword(use.name))
                continue;
            if (use.isEnum)
            {
                error = Format("shader {}: pass {} reads the enum keyword {}, which the shader does not declare", shader.name, pass.name, use.name);
                return false;
            }
            // A bool keyword declared elsewhere, such as a global one shared by many shaders.
            add(use.name, nullptr);
        }

        u64 count = 1;
        for (const Array<i64>& values : choices)
        {
            count *= values.Size();
            if (count > limit)
            {
                error = Format("shader {}: pass {} has more than {} variants; make keywords shader features or merge them into enums", shader.name,
                               pass.name, limit);
                return false;
            }
        }
        // Count through the combinations, the first keyword changing slowest.
        for (u64 index = 0; index < count; index++)
        {
            Array<i64>& row = out.variants.Emplace();
            row.Resize(choices.Size());
            u64 rest = index;
            for (usize k = choices.Size(); k-- > 0;)
            {
                row[k] = choices[k][usize(rest % choices[k].Size())];
                rest /= choices[k].Size();
            }
        }
        return true;
    }
}
