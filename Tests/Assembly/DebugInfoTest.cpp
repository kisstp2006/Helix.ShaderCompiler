#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Assembly/DebugInfo.h>
#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/Testing/Test.h>

#include <cstdio>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

HX_TEST(DebugInfo, FindsLinesInSideBySideAndEmbeddedPdbs)
{
    Assembly tests, library, includes;
    HX_REQUIRE(tests.Load(TestShadersImage()) && library.Load(ShaderLibraryImage()) && includes.Load(ShaderIncludesImage()));

    // The PDB next to the test shaders. The line numbers are those of Tests/Shaders/Basics.cs and Tests/Includes/Lighting.cs.
    DebugInfo side;
    HX_REQUIRE(side.Load(TestShadersSymbols()));
    HX_CHECK(side.Matches(tests) && !side.Matches(library) && !side.Matches(includes));
    String file;
    u32 line = 0;
    u32 add = tests.FindMethod(tests.FindType("Helix.ShaderTests", "Basics"), "Add");
    HX_REQUIRE(side.Find(add, 0, file, line));
    std::printf("    %s:%u\n", file.CStr(), line);
    HX_CHECK(StringView(file).EndsWith("Basics.cs") && line == 9);

    // The PDB the include assembly carries inside, compressed.
    DebugInfo embedded;
    HX_REQUIRE(embedded.LoadEmbedded(includes));
    HX_CHECK(embedded.Matches(includes) && !embedded.Matches(tests));
    u32 lambert = includes.FindMethod(includes.FindType("Helix.ShaderIncludes", "Lighting"), "Lambert");
    HX_REQUIRE(embedded.Find(lambert, 3, file, line));
    HX_CHECK(StringView(file).EndsWith("Lighting.cs") && line == 36);
    DebugInfo none;
    HX_CHECK(!none.LoadEmbedded(tests));

    // Damaged PDBs fail or find nothing, but never read outside the data.
    Span<const u8> pdb = TestShadersSymbols();
    for (usize size = 0; size < pdb.Size(); size += 1 + size / 8)
    {
        DebugInfo cut;
        if (cut.Load(Span<const u8>(pdb.Data(), size)))
            cut.Find(add, 0, file, line);
    }
}

HX_TEST(DebugInfo, ErrorsNameTheSourceLine)
{
    Assembly tests;
    HX_REQUIRE(tests.Load(TestShadersImage()));
    DebugInfo side;
    HX_REQUIRE(side.Load(TestShadersSymbols()));
    const DebugInfo* symbols[] = {&side};
    TranslateOptions options;
    options.symbols = Span<const DebugInfo* const>(symbols, 1);
    u32 rejected = tests.FindType("Helix.ShaderTests", "Rejected");
    Module module;
    String error;
    HX_CHECK(!TranslateEntryPoint(tests, tests.FindMethod(rejected, "PicksLambda"), Stage::Fragment, module, error, options));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("Rejected.cs:") && StringView(error).Contains(": PicksLambda: a delegate depends on a condition"));
    // A failure inside a called method names the line in that method.
    HX_CHECK(!TranslateEntryPoint(tests, tests.FindMethod(rejected, "StaticAutoProperty"), Stage::Fragment, module, error, options));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("Rejected.cs:") && StringView(error).Contains("get_Gain"));
    // Without debug information the message has no position.
    HX_CHECK(!TranslateEntryPoint(tests, tests.FindMethod(rejected, "PicksLambda"), Stage::Fragment, module, error));
    HX_CHECK(StringView(error).StartsWith("PicksLambda: "));
}
