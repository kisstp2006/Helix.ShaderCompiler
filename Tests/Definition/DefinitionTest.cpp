#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Backends/Glsl.h>
#include <Helix/ShaderCompiler/Definition/ShaderDefinition.h>
#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/ShaderCompiler/Variants/Variants.h>
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
    // Fog and Detail are constants of the variant, Flicker is read from the keyword block.
    HX_CHECK(module.keywords.Size() == 3u && module.keywords[2].dynamic && StringView(shader.source).Contains("* 0.5") &&
             StringView(shader.source).Contains(".z = 1.0") && StringView(shader.source).Contains("hx_block_hx_keywords"));
}

HX_TEST(Definition, ReadsSubShaders)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    ShaderDescription layered, glass;
    String error;
    bool ok = ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "Layered"), layered, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);
    HX_REQUIRE(layered.subShaders.Size() == 2u && layered.passes.Size() == 3u);
    HX_CHECK(layered.subShaders[0].lod == 400 && layered.subShaders[1].lod == 100 && FindTag(layered.subShaders[0].tags, "RenderType") &&
             layered.subShaders[1].tags.Empty());
    const ShaderPass& forward = layered.passes[0];
    const ShaderPass& outline = layered.passes[1];
    const ShaderPass& cheap = layered.passes[2];
    HX_CHECK(forward.subShader == 0 && outline.subShader == 0 && cheap.subShader == 1);
    // The detailed passes take the SubShader state unless they set their own.
    HX_CHECK(forward.state.cull == CullMode::Front && forward.state.zTest == CompareFunction::Less && forward.state.zWrite);
    HX_REQUIRE(forward.state.blend.Size() == 1u);
    HX_CHECK(forward.state.blend[0].source == BlendFactor::One && forward.state.stencil && forward.state.stencilReference == 2);
    HX_CHECK(outline.state.cull == CullMode::Back && !outline.state.zWrite && outline.state.zTest == CompareFunction::Less);
    HX_REQUIRE(outline.state.blend.Size() == 1u);
    HX_CHECK(outline.state.blend[0].source == BlendFactor::SrcAlpha && outline.state.stencil);
    HX_CHECK(cheap.state.cull == CullMode::Off && cheap.state.zTest == CompareFunction::LessEqual && cheap.state.blend.Empty() && !cheap.state.stencil);
    // Both SubShaders have a Forward pass; the first one is found.
    HX_CHECK(layered.FindPass("Forward") == &forward);

    // Without SubShader classes the passes form one SubShader with the LOD and tags of the shader.
    HX_REQUIRE(ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "Glass"), glass, error));
    HX_REQUIRE(glass.subShaders.Size() == 1u);
    HX_CHECK(glass.subShaders[0].lod == 300 && glass.subShaders[0].tags.Size() == 2u && glass.subShaders[0].type == 0);

    ShaderDescription bad;
    HX_CHECK(!ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "BadMix"), bad, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("next to SubShader classes"));
}

HX_TEST(Definition, TakesPassesWithUsePass)
{
    Assembly a, includes;
    HX_REQUIRE(a.Load(TestShadersImage()) && includes.Load(ShaderIncludesImage()));
    String error;
    // The shaders of the include assembly, then two of the test shaders, resolved together.
    Array<ShaderDescription> shaders;
    HX_REQUIRE(ReadShaderDefinitions(includes, shaders, error) && shaders.Size() == 1u && shaders[0].name == "Includes/Shadow");
    const char* names[] = {"Borrowing", "Glass"};
    for (const char* name : names)
    {
        ShaderDescription d;
        HX_REQUIRE(ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", name), d, error));
        shaders.Push(Move(d));
    }
    HX_CHECK(!shaders[1].passes[0].usePass.Empty() && shaders[1].passes[0].vertex == 0);
    bool ok = ResolveUsePasses(shaders, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);
    const ShaderDescription& borrowing = shaders[1];
    const ShaderDescription& glass = shaders[2];
    HX_REQUIRE(borrowing.passes.Size() == 3u);
    const ShaderPass& depth = borrowing.passes[0];
    const ShaderPass& shadow = borrowing.passes[1];
    HX_CHECK(depth.usePass.Empty() && depth.name == "Depth" && depth.lightMode == "DepthOnly" && depth.vertex == glass.FindPass("Depth")->vertex &&
             depth.state.colorMask == 0 && depth.assembly == &a);
    HX_CHECK(shadow.name == "ShadowCaster" && shadow.lightMode == "ShadowCaster" && shadow.assembly == &includes && shadow.subShader == 0);
    HX_CHECK(borrowing.passes[2].name == "Main" && borrowing.passes[2].assembly == &a);
    // Each shader knows the assembly that defines it.
    HX_CHECK(borrowing.assembly == &a && glass.assembly == &a);

    // The code of a borrowed pass is translated in its own assembly; its keywords make the variants.
    VariantSet set;
    ok = EnumerateVariants(a, borrowing, shadow, {}, set, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);
    HX_CHECK(set.keywords.Size() == 1u && set.keywords[0] == "Soft" && set.variants.Size() == 2u);

    // Missing shaders and passes and loops are errors.
    const char* bad[][2] = {{"UsesNothing", "no shader Tests/Nowhere"}, {"UsesNoPass", "has no pass OUTLINE"}, {"LoopA", "comes back to itself"}};
    for (const auto& b : bad)
    {
        Array<ShaderDescription> some;
        ShaderDescription d;
        HX_REQUIRE(ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", b[0]), d, error));
        some.Push(Move(d));
        const char* other = StringView(b[0]) == "LoopA" ? "LoopB" : "Glass";
        HX_REQUIRE(ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", other), d, error));
        some.Push(Move(d));
        HX_CHECK(!ResolveUsePasses(some, error));
        std::printf("    %s\n", error.CStr());
        HX_CHECK(StringView(error).Contains(b[1]));
    }
    // An unresolved UsePass cannot make variants.
    ShaderDescription unresolved;
    HX_REQUIRE(ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", "Borrowing"), unresolved, error));
    HX_CHECK(!EnumerateVariants(a, unresolved, unresolved.passes[0], {}, set, error) && StringView(error).Contains("not resolved"));
}

HX_TEST(Definition, ReadsComputeDefinitions)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    String error;
    ComputeDescription blur;
    bool ok = ReadComputeDefinition(a, a.FindType("Helix.ShaderTests", "Blur"), blur, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);
    HX_CHECK(blur.name == "Tests/Blur" && blur.assembly == &a && blur.keywords.Size() == 2u && blur.FindKeyword("Taps")->shaderFeature);
    // Kernels by name, in the order they are written, with their thread group sizes.
    HX_REQUIRE(blur.kernels.Size() == 2u);
    HX_CHECK(blur.FindKernel("Horizontal") == 0 && blur.FindKernel("Vertical") == 1 && blur.FindKernel("Helper") < 0 && !blur.HasKernel("Run"));
    const ComputeKernel& vertical = blur.kernels[1];
    HX_CHECK(vertical.groupSize[0] == 8 && vertical.groupSize[1] == 8 && vertical.groupSize[2] == 2 && blur.kernels[0].groupSize[0] == 64 &&
             blur.kernels[0].groupSize[1] == 1);
    HX_CHECK(vertical.method == a.FindMethod(a.FindType("Helix.ShaderTests", "Blur"), "Vertical"));

    Array<ComputeDescription> all;
    HX_CHECK(!ReadComputeDefinitions(a, all, error) && StringView(error).Contains("Tests/NoKernels"));
    const char* bad[][2] = {{"NoKernels", "no kernel"}, {"WithProperty", "material properties"}, {"Overloaded", "two kernels are named Run"}};
    for (const auto& b : bad)
    {
        ComputeDescription d;
        HX_CHECK(!ReadComputeDefinition(a, a.FindType("Helix.ShaderTests", b[0]), d, error));
        std::printf("    %s\n", error.CStr());
        HX_CHECK(StringView(error).Contains(b[1]));
    }
    ComputeDescription notCompute;
    HX_CHECK(!ReadComputeDefinition(a, a.FindType("Helix.ShaderTests", "Glass"), notCompute, error));
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
