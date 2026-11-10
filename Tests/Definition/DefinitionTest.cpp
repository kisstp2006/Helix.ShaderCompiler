#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Backends/Glsl.h>
#include <Helix/ShaderCompiler/Definition/ShaderDefinition.h>
#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/Testing/Test.h>

#include <cstdio>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

namespace
{
    const ShaderTag* FindTag(const Array<ShaderTag>& tags, StringView name)
    {
        for (const ShaderTag& t : tags)
            if (t.name == name)
                return &t;
        return nullptr;
    }
}

HX_TEST(Definition, ReadsShaderPropertiesAndKeywords)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    ShaderDescription glass;
    String error;
    bool ok = ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "Glass"), glass, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);
    HX_CHECK(glass.name == "Tests/Glass" && glass.fallback == "Tests/Lit" && glass.customEditor == "GlassEditor" && glass.lod == 300);
    HX_REQUIRE(glass.tags.Size() == 2u);
    HX_CHECK(FindTag(glass.tags, "Queue") && FindTag(glass.tags, "Queue")->value == "Transparent" && FindTag(glass.tags, "RenderType"));

    HX_REQUIRE(glass.properties.Size() == 7u);
    const ShaderProperty& tint = glass.properties[0];
    HX_CHECK(tint.name == "tint" && tint.displayName == "Tint" && tint.type == Type::Vector(BaseType::Float, 4));
    HX_CHECK(tint.Has(PropertyFlags::MainColor) && tint.Has(PropertyFlags::Hdr) && !tint.Has(PropertyFlags::MainTexture));
    HX_CHECK(tint.defaultValue[0] == 1.0f && tint.defaultValue[1] == 0.5f && tint.defaultValue[2] == 0.25f && tint.defaultValue[3] == 0.5f);
    const ShaderProperty* map = glass.FindProperty("baseMap");
    HX_REQUIRE(map != nullptr);
    HX_CHECK(map->type.base == BaseType::Texture2D && map->defaultTexture == "white" && map->displayName == "Base Map");
    HX_CHECK(map->Has(PropertyFlags::MainTexture) && map->Has(PropertyFlags::NoScaleOffset));
    const ShaderProperty* smoothness = glass.FindProperty("smoothness");
    HX_REQUIRE(smoothness != nullptr);
    HX_CHECK(smoothness->hasRange && smoothness->rangeMin == 0.0f && smoothness->rangeMax == 1.0f && smoothness->defaultValue[0] == 0.5f);
    HX_CHECK(glass.FindProperty("layers")->type == kInt && glass.FindProperty("layers")->Has(PropertyFlags::HideInInspector));
    HX_CHECK(glass.FindProperty("useFog")->Has(PropertyFlags::Toggle) && glass.FindProperty("useFog")->keyword == "Fog");
    HX_CHECK(glass.FindProperty("detail")->Has(PropertyFlags::KeywordEnum) && glass.FindProperty("detail")->keyword == "Detail");
    // A field without [Property] is still a property, shown under its own name.
    const ShaderProperty* offset = glass.FindProperty("offset");
    HX_REQUIRE(offset != nullptr);
    HX_CHECK(offset->displayName == "offset" && offset->Has(PropertyFlags::PerRendererData) && offset->type == Type::Vector(BaseType::Float, 2));

    HX_REQUIRE(glass.keywords.Size() == 3u);
    const ShaderKeyword* fog = glass.FindKeyword("Fog");
    const ShaderKeyword* detail = glass.FindKeyword("Detail");
    const ShaderKeyword* flicker = glass.FindKeyword("Flicker");
    HX_REQUIRE(fog && detail && flicker);
    HX_CHECK(fog->global && !fog->shaderFeature && !fog->dynamic && fog->values.Empty());
    HX_CHECK(!detail->global && detail->shaderFeature && !flicker->global && flicker->dynamic);
    HX_REQUIRE(detail->values.Size() == 3u);
    HX_CHECK(detail->valueNames[0] == "Low" && detail->valueNames[1] == "Medium" && detail->valueNames[2] == "High");
    HX_CHECK(detail->values[0] == 0 && detail->values[1] == 1 && detail->values[2] == 5);
}

HX_TEST(Definition, ReadsPassesWithRenderState)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    ShaderDescription glass;
    String error;
    HX_REQUIRE(ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "Glass"), glass, error));
    HX_REQUIRE(glass.passes.Size() == 2u);

    // Passes come in source order.
    const ShaderPass& depth = glass.passes[0];
    HX_CHECK(depth.name == "Depth" && depth.lightMode == "DepthOnly" && depth.tags.Empty());
    HX_CHECK(depth.state.cull == CullMode::Back && depth.state.zWrite && depth.state.zTest == CompareFunction::LessEqual && depth.state.zClip);
    HX_CHECK(depth.state.colorMask == 0 && depth.state.offsetFactor == 1.0f && depth.state.offsetUnits == 2.0f && depth.state.blend.Empty());
    HX_CHECK(depth.state.stencil && depth.state.stencilReference == 3 && depth.state.stencilWriteMask == 15 && depth.state.stencilReadMask == 255);
    HX_CHECK(depth.state.stencilFront.pass == StencilOperation::Replace && depth.state.stencilBack.pass == StencilOperation::Replace);

    const ShaderPass* forward = glass.FindPass("Forward");
    HX_REQUIRE(forward != nullptr && forward == &glass.passes[1]);
    const RenderState& s = forward->state;
    HX_CHECK(forward->lightMode == "ForwardBase" && FindTag(forward->tags, "Stage") && FindTag(forward->tags, "Stage")->value == "Blended");
    HX_CHECK(s.cull == CullMode::Off && !s.zWrite && s.zTest == CompareFunction::Less && !s.zClip && s.colorMask == 7 && s.alphaToMask &&
             s.conservative);
    HX_REQUIRE(s.blend.Size() == 2u);
    const BlendState& all = s.blend[0].target == -1 ? s.blend[0] : s.blend[1];
    const BlendState& second = s.blend[0].target == -1 ? s.blend[1] : s.blend[0];
    HX_CHECK(all.target == -1 && all.source == BlendFactor::SrcAlpha && all.destination == BlendFactor::OneMinusSrcAlpha &&
             all.sourceAlpha == BlendFactor::SrcAlpha && all.destinationAlpha == BlendFactor::OneMinusSrcAlpha && all.operation == BlendOperation::Add);
    HX_CHECK(second.target == 1 && second.source == BlendFactor::One && second.sourceAlpha == BlendFactor::Zero &&
             second.destinationAlpha == BlendFactor::One && second.operation == BlendOperation::Max && second.operationAlpha == BlendOperation::Subtract);
    HX_CHECK(s.stencil && s.stencilReadMask == 7);
    HX_CHECK(s.stencilFront.compare == CompareFunction::Equal && s.stencilFront.fail == StencilOperation::Zero &&
             s.stencilFront.zFail == StencilOperation::Keep);
    HX_CHECK(s.stencilBack.compare == CompareFunction::NotEqual && s.stencilBack.zFail == StencilOperation::Invert &&
             s.stencilBack.fail == StencilOperation::Keep);

    // The entry points of a pass compile, with the keywords of the definition selecting the variant.
    HX_REQUIRE(forward->vertex != 0 && forward->fragment != 0 && forward->vertex != depth.vertex);
    Module module;
    GlslShader shader;
    const KeywordValue keywords[] = {{"Fog", 1}, {"Detail", glass.FindKeyword("Detail")->values[2]}};
    TranslateOptions options;
    options.keywords = keywords;
    bool ok = TranslateEntryPoint(a, forward->vertex, Stage::Vertex, module, error) && EmitGlsl(module, GlslTarget::Glsl330, shader, error) &&
              TranslateEntryPoint(a, forward->fragment, Stage::Fragment, module, error, options) &&
              EmitGlsl(module, GlslTarget::Glsl330, shader, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);
    HX_CHECK(module.keywords.Size() == 2u && StringView(shader.source).Contains("* 0.5") && StringView(shader.source).Contains(".z = 1.0"));
}

HX_TEST(Definition, ReadsEveryShaderAndReportsBadOnes)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    String error;
    ShaderDescription lit, bad;
    HX_REQUIRE(ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "Lit"), lit, error));
    HX_CHECK(lit.name == "Tests/Lit" && lit.properties.Size() == 2u && lit.keywords.Size() == 1u && lit.passes.Size() == 1u);
    HX_CHECK(lit.passes[0].state.cull == CullMode::Front && !lit.passes[0].state.zWrite && lit.passes[0].lightMode == "ForwardBase");

    HX_CHECK(!ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "BadProperty"), bad, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("Tests/BadProperty") && StringView(error).Contains("label"));
    HX_CHECK(!ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "BadPass"), bad, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("pass Only") && StringView(error).Contains("FragmentShader"));
    HX_CHECK(!ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "BadToggle"), bad, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("Missing"));
    HX_CHECK(!ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "Basics"), bad, error));

    // Reading the whole assembly stops at the first definition that cannot work and names it.
    Array<ShaderDescription> all;
    HX_CHECK(!ReadShaderDefinitions(a, all, error));
    HX_CHECK(StringView(error).StartsWith("shader Tests/Bad"));
    // The shader library itself declares no shaders.
    Assembly library;
    HX_REQUIRE(library.Load(ShaderLibraryImage()));
    HX_CHECK(ReadShaderDefinitions(library, all, error) && all.Empty());
}
