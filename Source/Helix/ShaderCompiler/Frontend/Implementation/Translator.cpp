#include <Helix/ShaderCompiler/Frontend/Implementation/Translator.h>
#include <Helix/ShaderCompiler/Transforms/Simplify.h>

namespace helix::shadercompiler
{
    using frontend::Translator;


    bool TranslateEntryPoint(const Assembly& assembly, u32 method, Stage stage, Module& out, String& error, const TranslateOptions& options)
    {
        out = Module();
        out.stage = stage;
        error.Clear();
        Translator t(assembly, out, error, options);
        Array<Type> none;
        i32 index = t.FunctionFor(method, none, assembly.Method(method).name);
        if (index < 0)
        {
            if (error.Empty())
                error = Format("{}translation failed", t.At(0, method, 0));
            return false;
        }
        out.entry = u32(index);
        // Role of the return value: the attributes of the parameter row with sequence 0.
        MethodInfo info = assembly.Method(method);
        for (u32 p = info.paramBegin; p < info.paramEnd; p++)
            if (assembly.Param(p).sequence == 0)
            {
                bool flat;
                out.returnSemantic = t.ReadSemantic(Token(Table::Param, p), out.returnSemanticIndex, flat);
            }
        // Vertex buffers hold no 16 bit floats the shader could read as they lie in C#.
        if (stage == Stage::Vertex)
        {
            const Function& entry = out.functions[out.entry];
            for (const Variable& p : entry.params)
            {
                bool half = p.semantic == Semantic::Location && p.type.half;
                if (p.type.base == BaseType::Struct)
                    for (const StructField& f : out.structs[p.type.structIndex].fields)
                        half = half || (f.semantic == Semantic::Location && f.type.half);
                if (half)
                {
                    error = Format("{}{}: vertex input {} is half; vertex inputs take float", t.At(0, method, 0), info.name, p.name);
                    return false;
                }
            }
        }
        if (stage == Stage::Compute)
        {
            Array<AttributeArgument> size;
            if (!t.Has(Token(Table::MethodDef, method), "ComputeShaderAttribute", &size) || size.Size() != 3)
            {
                error = Format("{}{}: a compute shader needs [ComputeShader(x, y, z)]", t.At(0, method, 0), info.name);
                return false;
            }
            for (usize i = 0; i < 3; i++)
                out.groupSize[i] = u32(size[i].integer);
            if (!out.functions[out.entry].returnType.IsVoid())
            {
                error = Format("{}{}: a compute shader returns nothing; it writes buffers", t.At(0, method, 0), info.name);
                return false;
            }
        }
        // The keyword block takes its slot from user uniforms.
        bool keywordBlock = false;
        for (const Global& g : out.globals)
            keywordBlock = keywordBlock || g.name == StringView(kKeywordBlockName);
        for (const Global& g : out.globals)
            if (keywordBlock && g.kind == GlobalKind::Uniform && g.slot == kKeywordBlockSlot && g.name != StringView(kKeywordBlockName))
            {
                error = Format("{}{}: uniform {} is on slot {}, which holds the dynamic keywords", t.At(0, method, 0), info.name, g.name, kKeywordBlockSlot);
                return false;
            }
        Simplify(out);
        return true;
    }
}

