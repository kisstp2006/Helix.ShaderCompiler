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

    namespace
    {
        const ShaderKeyword* Declared(Span<const ShaderKeyword> declared, StringView name)
        {
            for (const ShaderKeyword& k : declared)
                if (k.name == name)
                    return &k;
            return nullptr;
        }

        // Keywords that make no variants (dynamic ones) are skipped; each keyword is listed once.
        void Collect(const Module& module, Array<KeywordUse>& read)
        {
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

        // The combinations of the values of the keywords the code read. `part` names the pass or kernel in messages.
        bool Choose(StringView shaderName, StringView part, Span<const ShaderKeyword> declared, const Array<KeywordUse>& read,
                    Span<const KeywordSelection> used, VariantSet& out, String& error, u32 limit)
        {
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
                                error = Format("shader {}: {} is not a value of the keyword {}", shaderName, value, name);
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
            for (const ShaderKeyword& keyword : declared)
                for (const KeywordUse& use : read)
                    if (use.name == keyword.name && !add(keyword.name, &keyword))
                        return false;
            for (const KeywordUse& use : read)
            {
                if (Declared(declared, use.name))
                    continue;
                if (use.isEnum)
                {
                    error = Format("shader {}: {} reads the enum keyword {}, which the shader does not declare", shaderName, part, use.name);
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
                    error = Format("shader {}: {} has more than {} variants; make keywords shader features or merge them into enums", shaderName,
                                   part, limit);
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

    bool EnumerateVariants(const Assembly& assembly, const ShaderDescription& shader, const ShaderPass& pass, Span<const KeywordSelection> used,
                           VariantSet& out, String& error, u32 limit)
    {
        out = VariantSet();
        error.Clear();
        if (!pass.usePass.Empty())
        {
            error = Format("shader {}: UsePass {} is not resolved; read the shaders with ReadShaderDefinitions or ResolveUsePasses", shader.name,
                           pass.usePass);
            return false;
        }
        // Translating with every keyword off reports the keywords the code reads, also those behind other keywords. A pass
        // taken with UsePass keeps the code of its own assembly.
        const Assembly& code = pass.assembly ? *pass.assembly : assembly;
        Array<KeywordUse> read;
        const u32 methods[2] = {pass.vertex, pass.fragment};
        for (u32 stage = 0; stage < 2; stage++)
        {
            Module module;
            if (!TranslateEntryPoint(code, methods[stage], stage == 0 ? Stage::Vertex : Stage::Fragment, module, error))
                return false;
            Collect(module, read);
        }
        return Choose(shader.name, Format("pass {}", pass.name), Span<const ShaderKeyword>(shader.keywords.Data(), shader.keywords.Size()), read, used,
                      out, error, limit);
    }

    bool EnumerateKernelVariants(const ComputeDescription& compute, const ComputeKernel& kernel, Span<const KeywordSelection> used, VariantSet& out,
                                 String& error, u32 limit)
    {
        out = VariantSet();
        error.Clear();
        if (!compute.assembly)
        {
            error = Format("shader {}: the compute description names no assembly", compute.name);
            return false;
        }
        Module module;
        if (!TranslateEntryPoint(*compute.assembly, kernel.method, Stage::Compute, module, error))
            return false;
        Array<KeywordUse> read;
        Collect(module, read);
        return Choose(compute.name, Format("kernel {}", kernel.name), Span<const ShaderKeyword>(compute.keywords.Data(), compute.keywords.Size()), read,
                      used, out, error, limit);
    }
}
