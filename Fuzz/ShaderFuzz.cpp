#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Assembly/DebugInfo.h>
#include <Helix/ShaderCompiler/Backends/Dxbc.h>
#include <Helix/ShaderCompiler/Backends/Glsl.h>
#include <Helix/ShaderCompiler/Backends/Hlsl.h>
#include <Helix/ShaderCompiler/Definition/ShaderDefinition.h>
#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/Testing/Fuzz.h>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

// A copy of one of the test images (the shader assembly, the library, the include assembly with its embedded PDB, or
// the PDB of the shaders) with a few bytes changed, then everything that reads it: the assembly reader, the PDB reader,
// the definition readers, the translation of every entry point and the three backends. Counts the entry points that
// translated.
HX_FUZZ(Assemblies)
{
    Span<const u8> images[] = {TestShadersImage(), ShaderLibraryImage(), ShaderIncludesImage(), TestShadersSymbols()};
    u32 which = round.Below(4);
    Array<u8> bytes;
    bytes.Append(images[which].Data(), images[which].Size());
    usize size = bytes.Size();
    bytes.Resize(size + 64);
    bytes.Resize(round.Mutate(bytes.Data(), size, bytes.Size()));
    Span<const u8> mutated(bytes.Data(), bytes.Size());

    round.Step(which == 0 ? "load the shaders" : which == 1 ? "load the library" : which == 2 ? "load the includes" : "load the pdb");
    Assembly tests, library, includes;
    bool ok = (which == 0 ? tests.Load(mutated) : tests.Load(TestShadersImage())) && (which == 1 ? library.Load(mutated) : library.Load(ShaderLibraryImage())) &&
              (which == 2 ? includes.Load(mutated) : includes.Load(ShaderIncludesImage()));
    DebugInfo symbols, embedded;
    bool sideBySide = which == 3 ? symbols.Load(mutated) : symbols.Load(TestShadersSymbols());
    embedded.LoadEmbedded(includes);
    if (!ok)
        return 0;
    const Assembly* references[] = {&library, &includes};
    const DebugInfo* debug[] = {&symbols, &embedded};
    TranslateOptions options;
    options.references = Span<const Assembly* const>(references, 2);
    options.symbols = Span<const DebugInfo* const>(debug, sideBySide ? 2 : 1);
    options.passes.verify = true;

    round.Step("definitions");
    Array<ShaderDescription> shaders;
    Array<ComputeDescription> computes;
    String error;
    ReadShaderDefinitions(tests, shaders, error, options.references);
    ReadComputeDefinitions(tests, computes, error);

    u64 translated = 0;
    for (u32 method = 1; method <= tests.RowCount(Table::MethodDef); method++)
    {
        Array<CustomAttribute> attributes;
        tests.Attributes(Token(Table::MethodDef, method), attributes);
        Stage stage = Stage::Vertex;
        bool entry = false;
        for (const CustomAttribute& c : attributes)
        {
            entry = entry || c.typeName == "VertexShaderAttribute" || c.typeName == "FragmentShaderAttribute" || c.typeName == "ComputeShaderAttribute";
            if (c.typeName == "FragmentShaderAttribute")
                stage = Stage::Fragment;
            if (c.typeName == "ComputeShaderAttribute")
                stage = Stage::Compute;
        }
        if (!entry)
            continue;
        round.Step("translate", String(tests.Method(method).name).CStr());
        String file;
        u32 line = 0;
        symbols.Find(method, round.Below(64), file, line);
        Module module;
        if (!TranslateEntryPoint(tests, method, stage, module, error, options))
            continue;
        translated++;
        GlslShader glsl;
        DxbcShader dxbc;
        String hlsl;
        round.Step("glsl");
        EmitGlsl(module, stage == Stage::Compute ? GlslTarget::Glsl430 : GlslTarget::Glsl330, glsl, error);
        round.Step("dxbc");
        EmitDxbc(module, dxbc, error);
        round.Step("hlsl");
        EmitHlsl(module, hlsl, error);
    }
    return translated;
}
