#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/ShaderCompiler/Reflection/Reflection.h>
#include <Helix/Testing/Test.h>

#include <cstdio>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

namespace
{
    bool ReflectMethod(const Assembly& a, u32 type, StringView method, Stage stage, ShaderReflection& out, String& error,
                       Span<const KeywordValue> keywords = {})
    {
        Module module;
        TranslateOptions options;
        options.keywords = keywords;
        bool ok = type != 0 && TranslateEntryPoint(a, a.FindMethod(type, method), stage, module, error, options) && Reflect(module, out, error);
        if (!ok)
            std::printf("    %s\n", error.CStr());
        return ok;
    }

    const ReflectedValue* FindValue(const Array<ReflectedValue>& values, Semantic semantic, u32 index = 0)
    {
        for (const ReflectedValue& v : values)
            if (v.semantic == semantic && v.index == index)
                return &v;
        return nullptr;
    }

    bool Member(const ReflectedBlock& block, StringView name, Type type, u32 offset, u32 size)
    {
        const ReflectedMember* m = block.Find(name);
        return m && m->type == type && m->offset == offset && m->size == size;
    }
}

HX_TEST(Reflection, DescribesBlocksResourcesAndStageValues)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    u32 forward = a.FindNestedType(a.FindType("Helix.ShaderTests", "Lit"), "Forward");
    ShaderReflection vertex, fragment;
    String error;
    HX_REQUIRE(ReflectMethod(a, forward, "Vertex", Stage::Vertex, vertex, error));
    HX_REQUIRE(ReflectMethod(a, forward, "Fragment", Stage::Fragment, fragment, error));
    Type float2 = Type::Vector(BaseType::Float, 2), float3 = Type::Vector(BaseType::Float, 3), float4 = Type::Vector(BaseType::Float, 4);

    // The vertex stage: one block, three inputs (two of them vertex attributes), three outputs.
    HX_REQUIRE(vertex.blocks.Size() == 1u && vertex.resources.Empty());
    const ReflectedBlock& draw = vertex.blocks[0];
    HX_CHECK(draw.name == "draw" && draw.slot == 2 && draw.size == 80u && draw.members.Size() == 2u);
    HX_CHECK(Member(draw, "modelViewProjection", Type::Matrix(4), 0, 64) && Member(draw, "tint", float4, 64, 16));
    HX_REQUIRE(vertex.inputs.Size() == 3u && vertex.outputs.Size() == 3u);
    HX_CHECK(FindValue(vertex.inputs, Semantic::Location, 0)->type == float3 && FindValue(vertex.inputs, Semantic::Location, 0)->name == "position");
    HX_CHECK(FindValue(vertex.inputs, Semantic::Location, 1)->type == float2 && FindValue(vertex.inputs, Semantic::InstanceId)->type == kUInt);
    HX_CHECK(FindValue(vertex.outputs, Semantic::Position) && FindValue(vertex.outputs, Semantic::Location, 1)->flat &&
             !FindValue(vertex.outputs, Semantic::Location, 0)->flat);

    // The fragment stage: the same block, a texture (its sampler comes with the slot), one target; Fog is the keyword
    // that decides it.
    HX_REQUIRE(fragment.blocks.Size() == 1u && fragment.resources.Size() == 1u);
    const ReflectedResource* albedo = fragment.FindResource("albedo");
    HX_REQUIRE(albedo != nullptr);
    HX_CHECK(albedo->kind == ResourceKind::Texture && albedo->slot == 1 && !fragment.FindResource("albedoSampler"));
    HX_CHECK(fragment.outputs.Size() == 1u && FindValue(fragment.outputs, Semantic::Target, 0)->type == float4);
    HX_CHECK(fragment.keywords.Size() == 1u && fragment.keywords[0].name == "Fog" && fragment.keywords[0].value == 0);

    PassReflection pass;
    bool ok = LinkReflection(vertex, fragment, pass, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);
    HX_CHECK(pass.blocks.Size() == 1u && pass.resources.Size() == 1u && pass.vertexInputs.Size() == 2u && pass.targets.Size() == 1u &&
             pass.keywords.Size() == 1u);

    // Where C# packs otherwise: the float3 after a float starts a new row, array elements take 16 bytes each.
    {
        ShaderReflection layouts;
        HX_REQUIRE(ReflectMethod(a, a.FindType("Helix.ShaderTests.Cases", "UniformLayouts"), "Fragment", Stage::Fragment, layouts, error));
        HX_REQUIRE(layouts.blocks.Size() == 1);
        const ReflectedBlock& p = layouts.blocks[0];
        const Type f1 = Type::Scalar(BaseType::Float), f2 = Type::Vector(BaseType::Float, 2), f3 = Type::Vector(BaseType::Float, 3);
        const Type i1 = Type::Scalar(BaseType::Int);
        HX_CHECK(Member(p, "weight", f1, 0, 4) && Member(p, "direction", f3, 16, 12) && Member(p, "weights[0]", f1, 32, 4) &&
                 Member(p, "weights[3]", f1, 80, 4) && Member(p, "after", f1, 96, 4) && Member(p, "offsets[0]", f2, 112, 8) &&
                 Member(p, "offsets[2]", f2, 144, 8) && Member(p, "counts[0]", i1, 160, 4) && Member(p, "counts[1]", i1, 176, 4) &&
                 Member(p, "tail", f2, 192, 8) && p.size == 208);
    }
    // Offsets follow std140; a three component vector takes 12 bytes and the float after it fills the row.
    ShaderReflection uniforms;
    HX_REQUIRE(ReflectMethod(a, a.FindType("Helix.ShaderTests.Cases", "Uniforms"), "Fragment", Stage::Fragment, uniforms, error));
    HX_REQUIRE(uniforms.FindBlock("data") != nullptr);
    const ReflectedBlock& data = *uniforms.FindBlock("data");
    HX_CHECK(data.slot == 0 && data.size == 112u);
    HX_CHECK(Member(data, "transform", Type::Matrix(4), 0, 64) && Member(data, "tint", float4, 64, 16) && Member(data, "offset", float2, 80, 8));
    HX_CHECK(Member(data, "scale", kFloat, 88, 4) && Member(data, "mode", kUInt, 92, 4) && Member(data, "steps", Type::Vector(BaseType::Int, 3), 96, 12) &&
             Member(data, "bias", kFloat, 108, 4));

    // Members of structs inside the block carry the path.
    ShaderReflection nested;
    HX_REQUIRE(ReflectMethod(a, a.FindType("Helix.ShaderTests", "Nested"), "Fragment", Stage::Fragment, nested, error));
    const ReflectedBlock& scene = nested.blocks[0];
    HX_CHECK(scene.slot == 4 && scene.size == 96u && scene.members.Size() == 8u);
    HX_CHECK(Member(scene, "ambient", float4, 0, 16) && Member(scene, "key.color", float4, 16, 16) && Member(scene, "key.range", float2, 32, 8) &&
             Member(scene, "fill.color", float4, 48, 16) && Member(scene, "fill.pad", float2, 72, 8) && Member(scene, "exposure", kFloat, 80, 4));
}

HX_TEST(Reflection, DescribesOneVariantAndComputeShaders)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    String error;
    // The texture of the Keywords case is only there in the variants that sample it.
    u32 keywords = a.FindType("Helix.ShaderTests.Cases", "Keywords");
    ShaderReflection off, on;
    const KeywordValue stripes[] = {{"Stripes", 1}};
    HX_REQUIRE(ReflectMethod(a, keywords, "Fragment", Stage::Fragment, off, error));
    HX_REQUIRE(ReflectMethod(a, keywords, "Fragment", Stage::Fragment, on, error, stripes));
    HX_CHECK(off.resources.Empty() && on.resources.Size() == 1u && on.FindResource("pattern") && off.keywords.Size() == 2u);

    ShaderReflection particles, paint, squares;
    HX_REQUIRE(ReflectMethod(a, a.FindType("Helix.ShaderTests.Kernels", "Particles"), "Run", Stage::Compute, particles, error));
    HX_CHECK(particles.stage == Stage::Compute && particles.groupSize[0] == 4 && particles.groupSize[1] == 4 && particles.groupSize[2] == 1);
    HX_CHECK(particles.inputs.Size() == 1u && particles.inputs[0].semantic == Semantic::DispatchThreadId && particles.outputs.Empty());
    const ReflectedResource* source = particles.FindResource("source");
    const ReflectedResource* moved = particles.FindResource("moved");
    HX_REQUIRE(source && moved);
    HX_CHECK(source->kind == ResourceKind::StorageBuffer && !source->writable && source->slot == 1 && source->stride == 32u &&
             source->element.base == BaseType::Struct);
    HX_CHECK(moved->writable && moved->slot == 2 && moved->stride == 32u);

    HX_REQUIRE(ReflectMethod(a, a.FindType("Helix.ShaderTests.Kernels", "Paint"), "Run", Stage::Compute, paint, error));
    HX_REQUIRE(paint.resources.Size() == 1u);
    HX_CHECK(paint.resources[0].kind == ResourceKind::StorageTexture && paint.resources[0].writable && paint.resources[0].format == StorageFormat::RGBA8 &&
             paint.resources[0].slot == 0);

    // A uniform block and a storage buffer may both use slot 0: they are different kinds of slots.
    HX_REQUIRE(ReflectMethod(a, a.FindType("Helix.ShaderTests.Kernels", "Squares"), "Run", Stage::Compute, squares, error));
    HX_CHECK(squares.blocks.Size() == 1u && squares.blocks[0].slot == 0 && squares.resources.Size() == 1u && squares.resources[0].slot == 0 &&
             squares.resources[0].stride == 4u);
}

HX_TEST(Reflection, RejectsStagesThatDoNotFit)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    String error;
    auto rhi = [&](StringView name) { return a.FindType("Helix.ShaderTests.Rhi", name); };
    ShaderReflection plain, textured, depth, colorFragment, placedFragment, colorVertex;
    HX_REQUIRE(ReflectMethod(a, rhi("Plain"), "Vertex", Stage::Vertex, plain, error));
    HX_REQUIRE(ReflectMethod(a, rhi("Textured"), "Vertex", Stage::Vertex, textured, error));
    HX_REQUIRE(ReflectMethod(a, rhi("Depth"), "Vertex", Stage::Vertex, depth, error));
    HX_REQUIRE(ReflectMethod(a, rhi("Color"), "Vertex", Stage::Vertex, colorVertex, error));
    HX_REQUIRE(ReflectMethod(a, rhi("Color"), "Fragment", Stage::Fragment, colorFragment, error));
    HX_REQUIRE(ReflectMethod(a, rhi("Placed"), "Fragment", Stage::Fragment, placedFragment, error));

    PassReflection pass;
    HX_CHECK(LinkReflection(colorVertex, colorFragment, pass, error));
    // The fragment stage reads a color nobody writes.
    HX_CHECK(!LinkReflection(plain, colorFragment, pass, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("location 0") && StringView(error).Contains("does not write"));
    // Location 0 is a float2 on one side and a float4 on the other.
    HX_CHECK(!LinkReflection(textured, colorFragment, pass, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("another type"));
    // Both stages bind slot 0, with blocks of different members.
    HX_CHECK(!LinkReflection(depth, placedFragment, pass, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("uniform slot 0"));
    // Stages in the wrong roles.
    HX_CHECK(!LinkReflection(colorFragment, colorVertex, pass, error));

    // A layout the GPU cannot take is reported by the reflection as well.
    ShaderReflection bad;
    Module module;
    HX_REQUIRE(TranslateEntryPoint(a, a.FindMethod(a.FindType("Helix.ShaderTests", "Rejected"), "HalfUniform"), Stage::Fragment, module, error));
    HX_CHECK(!Reflect(module, bad, error) && StringView(error).Contains("halfBlock.tint"));
}
