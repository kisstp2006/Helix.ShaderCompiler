#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Backends/Glsl.h>
#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/Testing/Test.h>

#include <cstdio>
#include <cstdlib>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

namespace
{
    bool Compile(const Assembly& a, u32 method, Stage stage, GlslTarget target, GlslShader& shader, String& error)
    {
        Module module;
        return TranslateEntryPoint(a, method, stage, module, error) && EmitGlsl(module, target, shader, error);
    }
}

HX_TEST(Glsl, EmitsVertexAndFragmentShaders)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    u32 forward = a.FindNestedType(a.FindType("Helix.ShaderTests", "Lit"), "Forward");
    GlslShader vertex, fragment;
    String error;
    bool ok = Compile(a, a.FindMethod(forward, "Vertex"), Stage::Vertex, GlslTarget::Glsl330, vertex, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);
    ok = Compile(a, a.FindMethod(forward, "Fragment"), Stage::Fragment, GlslTarget::Essl300, fragment, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);
    if (std::getenv("HELIX_PRINT_SHADERS"))
        std::printf("%s\n%s\n", vertex.source.CStr(), fragment.source.CStr());

    StringView vs = vertex.source, fs = fragment.source;
    HX_CHECK(vs.StartsWith("#version 330 core\n") && fs.StartsWith("#version 300 es\n"));
    HX_CHECK(vs.Contains("layout(location = 0) in vec3 hx_in0;") && vs.Contains("layout(location = 1) in vec2 hx_in1;"));
    HX_CHECK(vs.Contains("layout(std140) uniform hx_block_draw") && vs.Contains("gl_Position = r.m_position;"));
    HX_CHECK(vs.Contains("out vec2 hx_v0;") && vs.Contains("flat out uint hx_v1;") && vs.Contains("uint(gl_InstanceID)"));
    HX_CHECK(fs.Contains("in vec2 hx_v0;") && fs.Contains("flat in uint hx_v1;") && fs.Contains("gl_FragCoord"));
    HX_CHECK(fs.Contains("uniform sampler2D g_albedo;") && fs.Contains("texture(g_albedo, ") && fs.Contains("discard;"));
    HX_CHECK(fs.Contains("layout(location = 0) out vec4 hx_target0;"));
    // The Fog keyword is off, so its branch is a constant condition.
    HX_CHECK(fs.Contains("if (false)"));

    HX_REQUIRE(vertex.bindings.Size() == 1u && fragment.bindings.Size() == 2u);
    HX_CHECK(vertex.bindings[0].name == "hx_block_draw" && vertex.bindings[0].kind == GlobalKind::Uniform && vertex.bindings[0].slot == 2);
    bool texture = false;
    for (const GlslBinding& b : fragment.bindings)
        texture = texture || (b.name == "g_albedo" && b.kind == GlobalKind::Texture && b.slot == 1);
    HX_CHECK(texture);
}

HX_TEST(Glsl, ReportsUnsupportedCode)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    u32 basics = a.FindType("Helix.ShaderTests", "Basics");
    Module module;
    String error;
    // long and double arithmetic with 64 bit constants has no GPU form.
    HX_CHECK(!TranslateEntryPoint(a, a.FindMethod(basics, "Wide"), Stage::Fragment, module, error));
    HX_CHECK(StringView(error).StartsWith("Wide: "));
    std::printf("    %s\n", error.CStr());
    // A storage buffer is not supported by this backend yet; the message names what to do.
    u32 forward = a.FindNestedType(a.FindType("Helix.ShaderTests", "Lit"), "Forward");
    HX_CHECK(!TranslateEntryPoint(a, a.FindMethod(forward, "Count"), Stage::Fragment, module, error));
    std::printf("    %s\n", error.CStr());
}

HX_TEST(Glsl, RejectsWhatTheGpuCannotRun)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    u32 rejected = a.FindType("Helix.ShaderTests", "Rejected");
    HX_REQUIRE(rejected != 0);
    GlslShader shader;
    String error;
    HX_CHECK(!Compile(a, a.FindMethod(rejected, "Recursive"), Stage::Fragment, GlslTarget::Glsl330, shader, error));
    HX_CHECK(StringView(error).Contains("recursive"));
    std::printf("    %s\n", error.CStr());
    // float then float3: C# packs them, std140 aligns the vector to 16 bytes.
    HX_CHECK(!Compile(a, a.FindMethod(rejected, "Layout"), Stage::Fragment, GlslTarget::Glsl330, shader, error));
    HX_CHECK(StringView(error).Contains("settings.direction") && StringView(error).Contains("padding"));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(!Compile(a, a.FindMethod(rejected, "NoTarget"), Stage::Fragment, GlslTarget::Glsl330, shader, error));
    HX_CHECK(StringView(error).Contains("return value"));
    std::printf("    %s\n", error.CStr());
}
