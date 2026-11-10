#include <Support/TestAssemblies.h>

#include <Helix/Core/Strings/Format.h>
#include <Helix/Jobs/Jobs.h>
#include <Helix/Platform/IO/FileSystem.h>
#include <Helix/ShaderCompiler/Cache/ShaderCache.h>
#include <Helix/ShaderCompiler/Variants/Variants.h>
#include <Helix/Testing/Test.h>

#include <cstdio>
#include <cstring>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

namespace
{
    bool Same(const CompiledShader& a, const CompiledShader& b)
    {
        if (a.target != b.target || a.stage != b.stage || a.code.Size() != b.code.Size() || a.bindings.Size() != b.bindings.Size() ||
            std::memcmp(a.code.Data(), b.code.Data(), a.code.Size()) != 0)
            return false;
        for (usize i = 0; i < a.bindings.Size(); i++)
            if (a.bindings[i].name != b.bindings[i].name || a.bindings[i].kind != b.bindings[i].kind || a.bindings[i].slot != b.bindings[i].slot)
                return false;
        return true;
    }

    // Translates the fragment shader of a pass for one variant and compiles it through the cache.
    bool Compile(const Assembly& a, const ShaderPass& pass, const VariantSet& set, usize variant, ShaderTarget target, ShaderCache* cache,
                 CompiledShader& out, String& error)
    {
        Array<KeywordValue> values;
        set.Values(variant, values);
        TranslateOptions options;
        options.keywords = Span<const KeywordValue>(values.Data(), values.Size());
        Module module;
        return TranslateEntryPoint(a, pass.fragment, Stage::Fragment, module, error, options) && CompileModule(module, target, out, error, cache);
    }
}

HX_TEST(Variants, ListsTheCombinationsAPassReads)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    ShaderDescription glass, lit;
    String error;
    HX_REQUIRE(ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "Glass"), glass, error));
    HX_REQUIRE(ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "Lit"), lit, error));
    const ShaderPass* forward = glass.FindPass("Forward");
    const ShaderPass* depth = glass.FindPass("Depth");
    HX_REQUIRE(forward && depth);

    // Fog is a plain keyword: both values. Detail is a shader feature nobody uses: only its first member. Flicker is
    // dynamic: the code reads it, but it makes no variants.
    VariantSet set;
    bool ok = EnumerateVariants(a, glass, *forward, {}, set, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);
    HX_REQUIRE(set.keywords.Size() == 2u && set.variants.Size() == 2u);
    HX_CHECK(set.keywords[0] == "Fog" && set.keywords[1] == "Detail");
    HX_CHECK(set.variants[0][0] == 0 && set.variants[0][1] == 0 && set.variants[1][0] == 1 && set.variants[1][1] == 0);

    // Materials use Detail.High (5) and Medium (1): three values of it, six variants.
    const i64 details[] = {5, 1, 5};
    const KeywordSelection used[] = {{"Detail", details}, {"Flicker", details}};
    HX_REQUIRE(EnumerateVariants(a, glass, *forward, used, set, error));
    HX_REQUIRE(set.variants.Size() == 6u);
    HX_CHECK(set.variants[0][1] == 0 && set.variants[1][1] == 5 && set.variants[2][1] == 1 && set.variants[5][0] == 1 && set.variants[5][1] == 1);
    const KeywordValue wanted[] = {{"Fog", 1}, {"Detail", 5}, {"Other", 1}};
    i32 row = set.Find(wanted);
    HX_REQUIRE(row >= 0);
    HX_CHECK(set.variants[usize(row)][0] == 1 && set.variants[usize(row)][1] == 5);
    const KeywordValue missing[] = {{"Detail", 3}};
    HX_CHECK(set.Find(missing) < 0);
    Array<KeywordValue> values;
    set.Values(usize(row), values);
    HX_CHECK(values.Size() == 2u && values[0].name == "Fog" && values[0].value == 1 && values[1].value == 5);

    // A pass whose code reads no keyword has exactly one variant.
    HX_REQUIRE(EnumerateVariants(a, glass, *depth, used, set, error));
    HX_CHECK(set.keywords.Empty() && set.variants.Size() == 1u && set.variants[0].Empty());

    // In Lit, Fog is a shader feature: off only until a material turns it on.
    HX_REQUIRE(EnumerateVariants(a, lit, lit.passes[0], {}, set, error));
    HX_CHECK(set.variants.Size() == 1u && set.variants[0][0] == 0);
    const i64 on[] = {1};
    const KeywordSelection fog[] = {{"Fog", on}};
    HX_REQUIRE(EnumerateVariants(a, lit, lit.passes[0], fog, set, error));
    HX_CHECK(set.variants.Size() == 2u);

    const i64 wrong[] = {2};
    const KeywordSelection bad[] = {{"Detail", wrong}};
    HX_CHECK(!EnumerateVariants(a, glass, *forward, bad, set, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("Detail"));
    HX_CHECK(!EnumerateVariants(a, glass, *forward, used, set, error, 4));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("more than 4"));
}

HX_TEST(Cache, CompilesEveryDistinctVariantOnce)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    ShaderDescription glass;
    String error;
    HX_REQUIRE(ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "Glass"), glass, error));
    const ShaderPass* forward = glass.FindPass("Forward");
    const i64 details[] = {1, 5};
    const KeywordSelection used[] = {{"Detail", details}};
    VariantSet set;
    HX_REQUIRE(EnumerateVariants(a, glass, *forward, used, set, error) && set.variants.Size() == 6u);

    // The code only asks whether Detail is High, so Low and Medium give the same shader: four distinct ones.
    ShaderCache cache;
    Array<CompiledShader> first;
    for (usize v = 0; v < set.variants.Size(); v++)
        HX_REQUIRE(Compile(a, *forward, set, v, ShaderTarget::Glsl330, &cache, first.Emplace(), error));
    HX_CHECK(cache.Misses() == 4u && cache.MemoryHits() == 2u && cache.FileHits() == 0u);
    for (usize v = 0; v < set.variants.Size(); v++)
    {
        CompiledShader again, direct;
        HX_REQUIRE(Compile(a, *forward, set, v, ShaderTarget::Glsl330, &cache, again, error));
        HX_REQUIRE(Compile(a, *forward, set, v, ShaderTarget::Glsl330, nullptr, direct, error));
        HX_CHECK(Same(again, first[v]) && Same(direct, first[v]));
    }
    HX_CHECK(cache.Misses() == 4u && cache.MemoryHits() == 8u);
    HX_CHECK(first[0].Text().StartsWith("#version 330 core") && Same(first[0], first[1]) && !Same(first[0], first[2]) && !Same(first[0], first[3]));

    // Another target is another shader.
    CompiledShader bytecode;
    HX_REQUIRE(Compile(a, *forward, set, 0, ShaderTarget::Dxbc, &cache, bytecode, error));
    HX_CHECK(cache.Misses() == 5u && bytecode.code.Size() > 4u && std::memcmp(bytecode.code.Data(), "DXBC", 4) == 0);
    HX_REQUIRE(Compile(a, *forward, set, 0, ShaderTarget::Dxbc, &cache, bytecode, error));
    HX_CHECK(cache.Misses() == 5u && cache.MemoryHits() == 9u);
}

HX_TEST(Cache, CompilesKernelVariants)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    ComputeDescription blur;
    String error;
    HX_REQUIRE(ReadComputeDefinition(a, a.FindType("Helix.ShaderTests", "Blur"), blur, error));
    const ComputeKernel& horizontal = blur.kernels[usize(blur.FindKernel("Horizontal"))];
    const ComputeKernel& vertical = blur.kernels[usize(blur.FindKernel("Vertical"))];

    // Each kernel makes variants of the keywords its own code reads: Horizontal reads Wide, Vertical the shader feature Taps.
    VariantSet wide, taps;
    bool ok = EnumerateKernelVariants(blur, horizontal, {}, wide, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);
    HX_CHECK(wide.keywords.Size() == 1u && wide.keywords[0] == "Wide" && wide.variants.Size() == 2u);
    HX_REQUIRE(EnumerateKernelVariants(blur, vertical, {}, taps, error));
    HX_CHECK(taps.keywords.Size() == 1u && taps.keywords[0] == "Taps" && taps.variants.Size() == 1u);
    const i64 high[] = {5};
    const KeywordSelection used[] = {{"Taps", high}};
    HX_REQUIRE(EnumerateKernelVariants(blur, vertical, used, taps, error) && taps.variants.Size() == 2u);
    HX_CHECK(!EnumerateKernelVariants(blur, horizontal, {}, wide, error, 1) && StringView(error).Contains("kernel Horizontal has more than 1"));
    HX_REQUIRE(EnumerateKernelVariants(blur, horizontal, {}, wide, error));

    // Every variant compiles for the compute targets through the cache, and comes from it the second time.
    ShaderCache cache;
    const ShaderTarget targets[] = {ShaderTarget::Glsl430, ShaderTarget::Essl310, ShaderTarget::Dxbc};
    for (u32 round = 0; round < 2; round++)
        for (ShaderTarget target : targets)
            for (usize v = 0; v < wide.variants.Size(); v++)
            {
                Array<KeywordValue> values;
                wide.Values(v, values);
                TranslateOptions options;
                options.keywords = Span<const KeywordValue>(values.Data(), values.Size());
                Module module;
                CompiledShader compiled;
                HX_REQUIRE(TranslateEntryPoint(a, horizontal.method, Stage::Compute, module, error, options) &&
                           CompileModule(module, target, compiled, error, &cache));
                HX_CHECK(compiled.stage == Stage::Compute && compiled.groupSize[0] == 64 && compiled.groupSize[1] == 1 && !compiled.code.Empty());
                if (target == ShaderTarget::Essl310)
                    HX_CHECK(compiled.Text().StartsWith("#version 310 es"));
            }
    HX_CHECK(cache.Misses() == 6u && cache.MemoryHits() == 6u);
}

HX_TEST(Cache, SourceKeyCoversWhatTheTranslationReads)
{
    Span<const u8> image = TestShadersImage();
    Assembly a;
    HX_REQUIRE(a.Load(image));
    auto method = [&](const Assembly& from, StringView type) { return from.FindMethod(from.FindType("Helix.ShaderTests.Cases", type), "Fragment"); };
    TranslateOptions options;
    ShaderKey gradient = MakeSourceKey(a, method(a, "Gradient"), Stage::Fragment, options, ShaderTarget::Glsl330);
    ShaderKey arithmetic = MakeSourceKey(a, method(a, "Arithmetic"), Stage::Fragment, options, ShaderTarget::Glsl330);
    HX_CHECK(gradient == MakeSourceKey(a, method(a, "Gradient"), Stage::Fragment, options, ShaderTarget::Glsl330) && !(gradient == arithmetic));
    HX_CHECK(!(gradient == MakeSourceKey(a, method(a, "Gradient"), Stage::Fragment, options, ShaderTarget::Dxbc)));
    // Keyword values count, in any order.
    const KeywordValue on[] = {{"Stripes", 1}, {"Quality", 2}}, swapped[] = {{"Quality", 2}, {"Stripes", 1}}, off[] = {{"Stripes", 0}, {"Quality", 2}};
    TranslateOptions first, second, third;
    first.keywords = on;
    second.keywords = swapped;
    third.keywords = off;
    u32 keywords = method(a, "Keywords");
    HX_CHECK(MakeSourceKey(a, keywords, Stage::Fragment, first, ShaderTarget::Glsl330) == MakeSourceKey(a, keywords, Stage::Fragment, second, ShaderTarget::Glsl330));
    HX_CHECK(!(MakeSourceKey(a, keywords, Stage::Fragment, first, ShaderTarget::Glsl330) == MakeSourceKey(a, keywords, Stage::Fragment, third, ShaderTarget::Glsl330)));

    // A constant changed in the body of Arithmetic: its key changes, the key of Gradient, which does not reach it, stays.
    MethodBody body;
    HX_REQUIRE(a.Body(method(a, "Arithmetic"), body));
    Array<u8> edited;
    edited.Append(image.Data(), image.Size());
    usize at = usize(body.code.Data() - image.Data());
    bool changed = false;
    for (usize i = 0; i + 4 < body.code.Size() && !changed; i++)
        if (body.code[i] == 0x22) // ldc.r4
        {
            edited[at + i + 4] ^= 0x01;
            changed = true;
        }
    HX_REQUIRE(changed);
    Assembly b;
    HX_REQUIRE(b.Load(Span<const u8>(edited.Data(), edited.Size())));
    HX_CHECK(gradient == MakeSourceKey(b, method(b, "Gradient"), Stage::Fragment, options, ShaderTarget::Glsl330));
    HX_CHECK(!(arithmetic == MakeSourceKey(b, method(b, "Arithmetic"), Stage::Fragment, options, ShaderTarget::Glsl330)));

    // The second compile of an entry point is found by its source key: the frontend does not run again.
    ShaderCache cache;
    CompiledShader once, twice;
    String error;
    HX_REQUIRE(CompileEntryPoint(a, method(a, "Arithmetic"), Stage::Fragment, options, ShaderTarget::Glsl330, once, error, &cache));
    HX_CHECK(cache.Misses() == 2u && cache.MemoryHits() == 0u);
    HX_REQUIRE(CompileEntryPoint(a, method(a, "Arithmetic"), Stage::Fragment, options, ShaderTarget::Glsl330, twice, error, &cache));
    HX_CHECK(cache.Misses() == 2u && cache.MemoryHits() == 1u && Same(once, twice));
}

HX_TEST(Cache, CompilesTheVariantsOfAPassInParallel)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    ShaderDescription glass;
    String error;
    HX_REQUIRE(ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "Glass"), glass, error));
    const ShaderPass* forward = glass.FindPass("Forward");
    const i64 details[] = {1, 5};
    const KeywordSelection used[] = {{"Detail", details}};
    VariantSet set;
    HX_REQUIRE(EnumerateVariants(a, glass, *forward, used, set, error) && set.variants.Size() == 6u);
    TranslateOptions options;

    // One after the other, then in parallel into another cache: the same shaders.
    ShaderCache serialCache, parallelCache;
    Array<PassVariant> serial, parallel;
    HX_REQUIRE(!jobs::IsInitialized());
    bool ok = CompileVariants(a, *forward, set, ShaderTarget::Glsl330, options, serialCache, serial, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);
    HX_REQUIRE(jobs::Initialize({.workerCount = 4}));
    ok = CompileVariants(a, *forward, set, ShaderTarget::Glsl330, options, parallelCache, parallel, error);
    jobs::Shutdown();
    HX_REQUIRE(ok && serial.Size() == 6u && parallel.Size() == 6u);
    for (usize v = 0; v < 6; v++)
        HX_CHECK(Same(serial[v].vertex, parallel[v].vertex) && Same(serial[v].fragment, parallel[v].fragment) && !serial[v].fragment.code.Empty());
    // Low and Medium give the same fragment code: the module key shares it, whichever thread got there first.
    HX_CHECK(Same(serial[0].fragment, serial[2].fragment) || Same(serial[0].fragment, serial[1].fragment));
    // Two threads may both miss a module the other is compiling, so the parallel cache can count a few more misses.
    HX_CHECK(parallelCache.Misses() >= serialCache.Misses());
    // Asked again, every variant comes from its source keys.
    u32 hits = parallelCache.MemoryHits();
    HX_REQUIRE(CompileVariants(a, *forward, set, ShaderTarget::Glsl330, options, parallelCache, parallel, error));
    HX_CHECK(parallelCache.MemoryHits() == hits + 12u);
}

HX_TEST(Cache, KeepsShadersInADirectory)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    ShaderDescription lit;
    String error;
    HX_REQUIRE(ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "Lit"), lit, error));
    VariantSet set;
    HX_REQUIRE(EnumerateVariants(a, lit, lit.passes[0], {}, set, error));

    String directory = platform::JoinPath(platform::TempDirectory(), "helix_shader_cache_test");
    HX_REQUIRE(platform::CreateDirectories(directory));
    Array<String> files;
    auto list = [&]() {
        files.Clear();
        platform::ListDirectory(
            directory, [](const platform::DirectoryEntry& entry, void* user) { static_cast<Array<String>*>(user)->Push(String(entry.name)); }, &files);
    };
    list();
    for (const String& file : files)
        platform::RemoveFile(platform::JoinPath(directory, file));

    CompiledShader stored, loaded, repaired;
    {
        ShaderCache cache(directory);
        HX_REQUIRE(Compile(a, lit.passes[0], set, 0, ShaderTarget::Glsl450, &cache, stored, error));
        HX_CHECK(cache.Misses() == 1u && stored.bindings.Size() == 2u);
    }
    list();
    HX_REQUIRE(files.Size() == 1u);
    HX_CHECK(StringView(files[0]).EndsWith(".hxsc") && files[0].Size() == 37u);
    {
        // A new cache, as after a restart: the shader comes from the file, then from memory.
        ShaderCache cache(directory);
        HX_REQUIRE(Compile(a, lit.passes[0], set, 0, ShaderTarget::Glsl450, &cache, loaded, error));
        HX_CHECK(cache.FileHits() == 1u && cache.Misses() == 0u && Same(loaded, stored));
        HX_REQUIRE(Compile(a, lit.passes[0], set, 0, ShaderTarget::Glsl450, &cache, loaded, error));
        HX_CHECK(cache.FileHits() == 1u && cache.MemoryHits() == 1u && Same(loaded, stored));
    }
    // A damaged file is not trusted: the shader is compiled again and the file replaced.
    String path = platform::JoinPath(directory, files[0]);
    Array<u8> bytes;
    HX_REQUIRE(platform::ReadFile(path, bytes) && bytes.Size() > 64u);
    bytes[bytes.Size() / 2] ^= 0x40;
    HX_REQUIRE(platform::WriteFile(path, Span<const u8>(bytes.Data(), bytes.Size())));
    {
        ShaderCache cache(directory);
        HX_REQUIRE(Compile(a, lit.passes[0], set, 0, ShaderTarget::Glsl450, &cache, repaired, error));
        HX_CHECK(cache.FileHits() == 0u && cache.Misses() == 1u && Same(repaired, stored));
    }
    {
        ShaderCache cache(directory);
        HX_REQUIRE(Compile(a, lit.passes[0], set, 0, ShaderTarget::Glsl450, &cache, repaired, error));
        HX_CHECK(cache.FileHits() == 1u);
    }
    // A cut off file is damaged too.
    bytes.Resize(bytes.Size() / 2);
    HX_REQUIRE(platform::WriteFile(path, Span<const u8>(bytes.Data(), bytes.Size())));
    {
        ShaderCache cache(directory);
        HX_REQUIRE(Compile(a, lit.passes[0], set, 0, ShaderTarget::Glsl450, &cache, repaired, error));
        HX_CHECK(cache.FileHits() == 0u && cache.Misses() == 1u && Same(repaired, stored));
    }

    list();
    HX_CHECK(files.Size() == 1u);
    for (const String& file : files)
        platform::RemoveFile(platform::JoinPath(directory, file));
    platform::RemoveEmptyDirectory(directory);
}
