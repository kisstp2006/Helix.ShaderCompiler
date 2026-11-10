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
    // The Fog keyword is off: its branch is gone, and the module says the keyword decides this code.
    HX_CHECK(!fs.Contains("mix(") && !fs.Contains("if (false)"));

    HX_REQUIRE(vertex.bindings.Size() == 1u && fragment.bindings.Size() == 2u);
    HX_CHECK(vertex.bindings[0].name == "hx_block_draw" && vertex.bindings[0].kind == GlobalKind::Uniform && vertex.bindings[0].slot == 2);
    bool texture = false;
    for (const GlslBinding& b : fragment.bindings)
        texture = texture || (b.name == "g_albedo" && b.kind == GlobalKind::Texture && b.slot == 1);
    HX_CHECK(texture);
}

// A keyword is a constant of the variant: only the code, functions and resources of that variant are emitted.
HX_TEST(Glsl, KeywordsSelectTheVariant)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    u32 forward = a.FindNestedType(a.FindType("Helix.ShaderTests", "Lit"), "Forward");
    u32 keywords = a.FindType("Helix.ShaderTests.Cases", "Keywords");
    HX_REQUIRE(forward != 0 && keywords != 0);
    String error;
    auto compile = [&](u32 type, Span<const KeywordValue> values, Module& module, GlslShader& shader) {
        TranslateOptions options;
        options.keywords = values;
        bool ok = TranslateEntryPoint(a, a.FindMethod(type, "Fragment"), Stage::Fragment, module, error, options) &&
                  EmitGlsl(module, GlslTarget::Glsl330, shader, error);
        if (!ok)
            std::printf("    %s\n", error.CStr());
        return ok;
    };

    Module module;
    GlslShader shader;
    const KeywordValue fog[] = {{"Fog", 1}, {"NotInThisShader", 1}};
    HX_REQUIRE(compile(forward, fog, module, shader));
    HX_CHECK(StringView(shader.source).Contains("mix(") && !StringView(shader.source).Contains("if (true)"));
    HX_REQUIRE(module.keywords.Size() == 1u);
    HX_CHECK(module.keywords[0].name == "Fog" && !module.keywords[0].isEnum && module.keywords[0].value == 1);

    // Everything off: no texture, no helper function, one function left.
    HX_REQUIRE(compile(keywords, {}, module, shader));
    HX_CHECK(module.functions.Size() == 1u && module.globals.Empty() && shader.bindings.Empty());
    HX_CHECK(!StringView(shader.source).Contains("texture(") && !StringView(shader.source).Contains("for (") &&
             !StringView(shader.source).Contains("if ("));
    HX_REQUIRE(module.keywords.Size() == 2u);
    HX_CHECK(module.keywords[0].name == "Stripes" && module.keywords[1].name == "Quality" && module.keywords[1].isEnum);

    // Stripes on, Quality High (2): the texture with its sampler and the helper are back.
    const KeywordValue high[] = {{"Stripes", 1}, {"Quality", 2}};
    HX_REQUIRE(compile(keywords, high, module, shader));
    HX_CHECK(module.functions.Size() == 2u && shader.bindings.Size() == 1u && StringView(shader.source).Contains("texture("));
    HX_CHECK(module.keywords[1].value == 2);
    if (std::getenv("HELIX_PRINT_SHADERS"))
        std::printf("%s\n", shader.source.CStr());
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
    // A compute shader does not fit the GLSL versions that have none.
    u32 forward = a.FindNestedType(a.FindType("Helix.ShaderTests", "Lit"), "Forward");
    GlslShader shader;
    HX_REQUIRE(TranslateEntryPoint(a, a.FindMethod(forward, "Count"), Stage::Compute, module, error));
    HX_CHECK(!EmitGlsl(module, GlslTarget::Glsl330, shader, error) && StringView(error).Contains("4.30"));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(!EmitGlsl(module, GlslTarget::Essl300, shader, error));
    // The same method is not a fragment shader: it has no outputs to return.
    HX_CHECK(TranslateEntryPoint(a, a.FindMethod(forward, "Count"), Stage::Fragment, module, error) &&
             !EmitGlsl(module, GlslTarget::Glsl450, shader, error));
    std::printf("    %s\n", error.CStr());
}

HX_TEST(Glsl, EmitsComputeShadersWithStorageBuffers)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    u32 forward = a.FindNestedType(a.FindType("Helix.ShaderTests", "Lit"), "Forward");
    Module module;
    GlslShader desktop, mobile;
    String error;
    bool ok = TranslateEntryPoint(a, a.FindMethod(forward, "Count"), Stage::Compute, module, error) &&
              EmitGlsl(module, GlslTarget::Glsl430, desktop, error) && EmitGlsl(module, GlslTarget::Essl310, mobile, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);
    HX_CHECK(module.groupSize[0] == 8 && module.groupSize[1] == 4 && module.groupSize[2] == 1);
    HX_REQUIRE(module.globals.Size() == 1u);
    HX_CHECK(module.globals[0].kind == GlobalKind::Storage && module.globals[0].writable && module.globals[0].element == kUInt &&
             module.globals[0].slot == 3);
    StringView d = desktop.source, e = mobile.source;
    HX_CHECK(d.StartsWith("#version 430 core\n") && e.StartsWith("#version 310 es\n"));
    HX_CHECK(d.Contains("layout(std430, binding = 3) buffer hx_buffer_counters") && d.Contains("uint g_counters[];"));
    HX_CHECK(d.Contains("layout(local_size_x = 8, local_size_y = 4, local_size_z = 1) in;") && d.Contains("uvec3(gl_GlobalInvocationID)"));
    HX_CHECK(d.Contains("g_counters[") && e.Contains("g_counters["));

    // Elements must lie the same way in C#, in std430 and packed.
    u32 rejected = a.FindType("Helix.ShaderTests", "Rejected");
    HX_REQUIRE(TranslateEntryPoint(a, a.FindMethod(rejected, "PaddedElements"), Stage::Compute, module, error));
    HX_CHECK(!EmitGlsl(module, GlslTarget::Glsl430, desktop, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("points") && StringView(error).Contains("three component"));
    HX_CHECK(!TranslateEntryPoint(a, a.FindMethod(rejected, "CopiesBuffer"), Stage::Compute, module, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("directly"));
    HX_CHECK(!TranslateEntryPoint(a, a.FindMethod(rejected, "ReadsColors"), Stage::Compute, module, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("R32F, R32UI or R32I"));
    HX_CHECK(TranslateEntryPoint(a, a.FindMethod(rejected, "HalfUniform"), Stage::Fragment, module, error) &&
             !EmitGlsl(module, GlslTarget::Glsl330, desktop, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("halfBlock.tint") && StringView(error).Contains("float"));
    HX_CHECK(!TranslateEntryPoint(a, a.FindMethod(rejected, "HalfInput"), Stage::Vertex, module, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("vertex input p is half"));

    // Half values are medium precision on GLSL ES and plain floats elsewhere.
    u32 halves = a.FindType("Helix.ShaderTests.Cases", "HalfMath");
    HX_REQUIRE(TranslateEntryPoint(a, a.FindMethod(halves, "Fragment"), Stage::Fragment, module, error) &&
               EmitGlsl(module, GlslTarget::Glsl330, desktop, error) && EmitGlsl(module, GlslTarget::Essl300, mobile, error));
    HX_CHECK(!StringView(desktop.source).Contains("mediump") && StringView(mobile.source).Contains("mediump vec3") &&
             StringView(mobile.source).Contains("out mediump vec4 hx_target0"));
    HX_CHECK(!TranslateEntryPoint(a, a.FindMethod(rejected, "WrongFormat"), Stage::Compute, module, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("does not fit"));
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
