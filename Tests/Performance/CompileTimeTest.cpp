#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Cache/ShaderCache.h>
#include <Helix/ShaderCompiler/Definition/ShaderDefinition.h>
#include <Helix/ShaderCompiler/Variants/Variants.h>
#include <Helix/Testing/Test.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

namespace
{
    // Milliseconds of one compile of the variant through an empty cache: both stages translated from the loaded IL,
    // linked, optimized and emitted.
    bool Time(const Assembly& a, const ShaderPass& pass, const VariantSet& set, ShaderTarget target, const TranslateOptions& options, double& ms, usize& bytes,
              String& error)
    {
        ShaderCache cache;
        Array<PassVariant> out;
        auto start = std::chrono::steady_clock::now();
        bool ok = CompileVariants(a, pass, set, target, options, cache, out, error);
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (!ok || out.Size() != 1u)
            return false;
        bytes = out[0].vertex.code.Size() + out[0].fragment.code.Size();
        return !out[0].vertex.code.Empty() && !out[0].fragment.code.Empty();
    }
}

// The compile time target of the shader compiler: one variant of a typical lit shader under 15 ms for GLSL and 25 ms
// for DXBC from loaded IL. Printed on every run (so it is in the CI output); the limits hold for optimized builds.
HX_TEST(Performance, CompilesAVariantOfATypicalLitShaderInTime)
{
    Assembly a, library;
    HX_REQUIRE(a.Load(TestShadersImage()) && library.Load(ShaderLibraryImage()));
    const Assembly* list[] = {&library};
    TranslateOptions options;
    options.references = Span<const Assembly* const>(list, 1);
    ShaderDescription lit;
    String error;
    HX_REQUIRE(ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "TypicalLit"), lit, error));
    const ShaderPass* forward = lit.FindPass("Forward");
    HX_REQUIRE(forward != nullptr);

    // The heaviest variant: normal map, shadows and fog on.
    const i64 on[] = {1};
    const KeywordSelection used[] = {{"NormalMap", on}};
    VariantSet all;
    HX_REQUIRE(EnumerateVariants(a, lit, *forward, used, all, error));
    HX_REQUIRE(all.keywords.Size() == 3u);
    const KeywordValue heaviest[] = {{"NormalMap", 1}, {"Shadows", 1}, {"Fog", 1}};
    i32 row = all.Find(Span<const KeywordValue>(heaviest, 3));
    HX_REQUIRE(row >= 0);
    VariantSet one;
    one.keywords = all.keywords;
    one.variants.Push(all.variants[usize(row)]);

    struct Target
    {
        ShaderTarget target;
        const char* name;
        double limit;
    };
    const Target targets[] = {{ShaderTarget::Glsl450, "GLSL 450", 15.0}, {ShaderTarget::Dxbc, "DXBC", 25.0}};
    for (const Target& t : targets)
    {
        constexpr int kRuns = 9;
        double runs[kRuns];
        usize bytes = 0;
        for (int i = 0; i < kRuns; i++)
        {
            bool ok = Time(a, *forward, one, t.target, options, runs[i], bytes, error);
            if (!ok)
                std::printf("    %s: %s\n", t.name, error.CStr());
            HX_REQUIRE(ok);
        }
        std::sort(runs, runs + kRuns);
        std::printf("    %-8s best %7.2f ms, median %7.2f ms (target %.0f ms), %zu bytes of code\n", t.name, runs[0], runs[kRuns / 2], t.limit,
                    bytes);
#ifdef NDEBUG
        HX_CHECK(runs[kRuns / 2] < t.limit);
#endif
    }
}
