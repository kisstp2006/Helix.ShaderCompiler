#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Assembly/AssemblyResolver.h>
#include <Helix/Testing/Test.h>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

HX_TEST(AssemblyResolver, FindsAddedAssembliesByName)
{
    Assembly tests, library, includes;
    HX_REQUIRE(tests.Load(TestShadersImage()) && library.Load(ShaderLibraryImage()) && includes.Load(ShaderIncludesImage()));
    AssemblyResolver resolver;
    String missing;
    HX_CHECK(!resolver.LoadReferences(tests, missing));
    HX_CHECK(missing == "HelixEngine.Shaders" || missing == "Helix.ShaderIncludes");

    resolver.Add(library);
    resolver.Add(includes);
    HX_CHECK(resolver.LoadReferences(tests, missing) && missing.Empty());
    HX_CHECK(resolver.Find("Helix.ShaderIncludes") == &includes);
    HX_CHECK(resolver.Find("Nothing.Like.This") == nullptr);
    HX_CHECK(resolver.References().Size() == 2);
}

// Web and Android runs have no access to the build directory.
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
HX_TEST(AssemblyResolver, LoadsReferencesFromDirectories)
{
    Assembly tests;
    HX_REQUIRE(tests.Load(TestShadersImage()));
    AssemblyResolver resolver;
    resolver.AddDirectory(HELIX_TEST_ASSEMBLY_DIRECTORY);
    String missing;
    HX_REQUIRE(resolver.LoadReferences(tests, missing));
    const Assembly* library = resolver.Find("HelixEngine.Shaders");
    const Assembly* includes = resolver.Find("Helix.ShaderIncludes");
    HX_REQUIRE(library && includes);
    HX_CHECK(library->FindType("HelixEngine.Shaders", "ShaderMath") != 0);
    HX_CHECK(includes->FindType("Helix.ShaderIncludes", "Lighting") != 0);
    HX_CHECK(resolver.References().Size() == 2);
}
#endif
