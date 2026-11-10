#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Backends/Hlsl.h>
#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/Testing/Test.h>

#include <cstdio>
#include <cstdlib>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

namespace
{
    bool Hlsl(const Assembly& a, StringView ns, StringView type, StringView method, Stage stage, String& out)
    {
        Assembly library, includes;
        library.Load(ShaderLibraryImage());
        includes.Load(ShaderIncludesImage());
        const Assembly* references[] = {&library, &includes};
        TranslateOptions options;
        options.references = Span<const Assembly* const>(references, 2);
        Module module;
        String error;
        bool ok = TranslateEntryPoint(a, a.FindMethod(a.FindType(ns, type), method), stage, module, error, options) && EmitHlsl(module, out, error);
        if (!ok)
            std::printf("    %s: %s\n", String(type).CStr(), error.CStr());
        return ok;
    }
}

HX_TEST(Hlsl, WritesReadableShaderModel5)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    String text;
    // Resources keep their slots as registers; textures come with the sampler of their slot.
    HX_REQUIRE(Hlsl(a, "Helix.ShaderTests.Cases", "Textures", "Fragment", Stage::Fragment, text));
    HX_CHECK(StringView(text).Contains("Texture2D<float4> g_") && StringView(text).Contains(": register(t0)") &&
             StringView(text).Contains("SamplerState g_") && StringView(text).Contains(".Sample(") && StringView(text).Contains(": SV_Target0"));
    HX_REQUIRE(Hlsl(a, "Helix.ShaderTests.Cases", "Uniforms", "Fragment", Stage::Fragment, text));
    HX_CHECK(StringView(text).Contains("cbuffer hx_block_") && StringView(text).Contains(": register(b"));
    // Matrices are the transposes of the C# ones, so the operands of mul swap.
    HX_REQUIRE(Hlsl(a, "Helix.ShaderTests.Cases", "Matrices", "Fragment", Stage::Fragment, text));
    HX_CHECK(StringView(text).Contains("mul(") && StringView(text).Contains("float4x4"));
    HX_REQUIRE(Hlsl(a, "Helix.ShaderTests.Kernels", "Histogram", "Run", Stage::Compute, text));
    HX_CHECK(StringView(text).Contains("[numthreads(") && StringView(text).Contains("SV_DispatchThreadID") &&
             StringView(text).Contains("InterlockedAdd(b[i], v, original)") && StringView(text).Contains("RWStructuredBuffer<uint>"));
    HX_REQUIRE(Hlsl(a, "Helix.ShaderTests.Cases", "Fullscreen", "Vertex", Stage::Vertex, text));
    HX_CHECK(StringView(text).Contains(": SV_Position") && StringView(text).Contains(": TEXCOORD0"));
    // HELIX_PRINT_HLSL=<case> shows the text of one case.
    if (const char* name = std::getenv("HELIX_PRINT_HLSL"); name && Hlsl(a, "Helix.ShaderTests.Cases", name, "Fragment", Stage::Fragment, text))
        std::printf("%s\n", text.CStr());

    // Every case and kernel has an HLSL form.
    u32 written = 0;
    for (u32 row = 1; row <= a.RowCount(Table::TypeDef); row++)
    {
        TypeDefInfo type = a.TypeDef(row);
        bool kernel = type.nameSpace == StringView("Helix.ShaderTests.Kernels");
        if (!kernel && type.nameSpace != StringView("Helix.ShaderTests.Cases"))
            continue;
        if (!a.FindMethod(row, kernel ? "Run" : "Fragment"))
            continue;
        HX_CHECK(Hlsl(a, type.nameSpace, type.name, kernel ? "Run" : "Fragment", kernel ? Stage::Compute : Stage::Fragment, text));
        written++;
    }
    HX_CHECK(written >= 40);
}
