#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Backends/Dxbc.h>
#include <Helix/ShaderCompiler/Backends/Glsl.h>
#include <Helix/ShaderCompiler/Backends/Hlsl.h>
#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/Testing/Test.h>

#include <cstdio>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

namespace
{
    bool HasAttribute(const Assembly& a, u32 method, StringView name)
    {
        Array<CustomAttribute> attributes;
        a.Attributes(Token(Table::MethodDef, method), attributes);
        for (const CustomAttribute& c : attributes)
            if (c.typeName == name)
                return true;
        return false;
    }

    // The class a nested class is declared in, at the top.
    u32 Outermost(const Assembly& a, u32 type)
    {
        while (a.TypeDef(type).enclosingType)
            type = a.TypeDef(type).enclosingType;
        return type;
    }
}

// Every entry point of the test shaders (cases, kernels, the shaders of the Helix.RHI tests and the definitions) compiles
// for every target that can hold it.
HX_TEST(Corpus, CompilesEveryEntryPointForEveryTarget)
{
    Assembly a, library, includes;
    HX_REQUIRE(a.Load(TestShadersImage()) && library.Load(ShaderLibraryImage()) && includes.Load(ShaderIncludesImage()));
    const Assembly* references[] = {&library, &includes};
    TranslateOptions options;
    options.references = Span<const Assembly* const>(references, 2);
    options.passes.verify = true;
    static constexpr GlslTarget kGraphics[] = {GlslTarget::Glsl330, GlslTarget::Glsl450, GlslTarget::Essl300, GlslTarget::Essl310, GlslTarget::Essl320};
    static constexpr GlslTarget kCompute[] = {GlslTarget::Glsl430, GlslTarget::Glsl450, GlslTarget::Essl310, GlslTarget::Essl320};
    u32 entryPoints = 0, compiled = 0, failed = 0;
    for (u32 method = 1; method <= a.RowCount(Table::MethodDef); method++)
    {
        bool vertex = HasAttribute(a, method, "VertexShaderAttribute"), fragment = HasAttribute(a, method, "FragmentShaderAttribute"),
             compute = HasAttribute(a, method, "ComputeShaderAttribute");
        if (!vertex && !fragment && !compute)
            continue;
        // Shaders written to be refused are tested on their own.
        StringView owner = a.TypeDef(Outermost(a, a.DeclaringTypeOfMethod(method))).name;
        if (owner == "Rejected")
            continue;
        entryPoints++;
        Stage stage = vertex ? Stage::Vertex : fragment ? Stage::Fragment : Stage::Compute;
        Module module;
        String error;
        if (!TranslateEntryPoint(a, method, stage, module, error, options))
        {
            std::printf("    %s.%s: %s\n", String(owner).CStr(), String(a.Method(method).name).CStr(), error.CStr());
            failed++;
            continue;
        }
        BindlessUse bindless = FindBindless(module);
        for (GlslTarget target : compute ? Span<const GlslTarget>(kCompute) : Span<const GlslTarget>(kGraphics))
        {
            // Bindless textures need GLSL 4.50 with the extension, bindless buffers desktop GLSL 4.30 and up.
            if ((bindless.textures && target != GlslTarget::Glsl450) || (bindless.buffers && (IsEssl(target) || target == GlslTarget::Glsl330)))
                continue;
            GlslShader glsl;
            if (!EmitGlsl(module, target, glsl, error))
            {
                std::printf("    %s.%s (GLSL target %u): %s\n", String(owner).CStr(), String(a.Method(method).name).CStr(), u32(target), error.CStr());
                failed++;
                continue;
            }
            compiled++;
        }
        DxbcShader dxbc;
        String hlsl;
        if (!EmitDxbc(module, dxbc, error) || !EmitHlsl(module, hlsl, error))
        {
            std::printf("    %s.%s (Direct3D): %s\n", String(owner).CStr(), String(a.Method(method).name).CStr(), error.CStr());
            failed++;
            continue;
        }
        compiled++;
    }
    std::printf("    %u entry points, %u compiled shaders, %u failures\n", entryPoints, compiled, failed);
    HX_CHECK(entryPoints >= 100 && failed == 0);
}
