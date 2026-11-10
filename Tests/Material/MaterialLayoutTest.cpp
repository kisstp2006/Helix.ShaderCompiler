#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/ShaderCompiler/Material/MaterialLayout.h>
#include <Helix/Testing/Test.h>

#include <cstdio>
#include <cstring>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

namespace
{
    // Reads a shader, compiles the two stages of one pass and links their reflection.
    bool Prepare(const Assembly& a, StringView type, StringView passName, ShaderDescription& shader, PassReflection& pass, String& error)
    {
        if (!ReadShaderDefinition(a, a.FindType("Helix.ShaderTests", type), shader, error))
            return false;
        const ShaderPass* found = shader.FindPass(passName);
        if (!found)
            return false;
        Module vertexModule, fragmentModule;
        ShaderReflection vertex, fragment;
        return TranslateEntryPoint(a, found->vertex, Stage::Vertex, vertexModule, error) &&
               TranslateEntryPoint(a, found->fragment, Stage::Fragment, fragmentModule, error) && Reflect(vertexModule, vertex, error) &&
               Reflect(fragmentModule, fragment, error) && LinkReflection(vertex, fragment, pass, error);
    }

    u32 PropertyIndex(const ShaderDescription& shader, StringView name)
    {
        for (u32 i = 0; i < shader.properties.Size(); i++)
            if (shader.properties[i].name == name)
                return i;
        return ~0u;
    }

    template <typename T>
    T At(const Array<u8>& bytes, u32 offset)
    {
        T v{};
        if (usize(offset) + sizeof(T) <= bytes.Size())
            std::memcpy(&v, bytes.Data() + offset, sizeof(T));
        return v;
    }
}

HX_TEST(Material, TiesPropertiesToBlockMembersAndTextures)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    ShaderDescription shader;
    PassReflection pass;
    String error;
    bool ok = Prepare(a, "Painted", "Forward", shader, pass, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);
    MaterialLayout layout;
    ok = BindProperties(shader, pass, layout, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);

    // Only the block that takes properties is a material block; the frame block belongs to the renderer.
    HX_REQUIRE(layout.blocks.Size() == 1u);
    const MaterialBlock& block = layout.blocks[0];
    HX_CHECK(block.name == "material" && block.slot == 1 && block.defaults.Size() == 48u && block.propertiesOnly);

    u32 tint = PropertyIndex(shader, "tint"), baseMap = PropertyIndex(shader, "baseMap"), detailMap = PropertyIndex(shader, "detailMap");
    u32 smoothness = PropertyIndex(shader, "smoothness"), layers = PropertyIndex(shader, "layers"), mask = PropertyIndex(shader, "mask");
    const PropertyBinding* b = layout.Find(tint, PropertyTarget::Member);
    HX_REQUIRE(b != nullptr);
    HX_CHECK(b->block == 0 && b->offset == 0 && b->size == 16u);
    HX_REQUIRE(layout.Find(smoothness, PropertyTarget::Member) && layout.Find(layers, PropertyTarget::Member) && layout.Find(mask, PropertyTarget::Member));
    HX_CHECK(layout.Find(smoothness, PropertyTarget::Member)->offset == 32u && layout.Find(layers, PropertyTarget::Member)->offset == 36u &&
             layout.Find(mask, PropertyTarget::Member)->offset == 40u);

    // Textures go to their slots; the tiling of baseMap has a member, detailMap takes none.
    HX_REQUIRE(layout.Find(baseMap, PropertyTarget::Texture) && layout.Find(detailMap, PropertyTarget::Texture));
    HX_CHECK(layout.Find(baseMap, PropertyTarget::Texture)->slot == 0 && layout.Find(detailMap, PropertyTarget::Texture)->slot == 3);
    HX_REQUIRE(layout.Find(baseMap, PropertyTarget::ScaleOffset) != nullptr);
    HX_CHECK(layout.Find(baseMap, PropertyTarget::ScaleOffset)->offset == 16u && !layout.Find(detailMap, PropertyTarget::ScaleOffset));
    HX_CHECK(layout.unused.Size() == 1u && layout.unused[0] == PropertyIndex(shader, "spare"));

    // The defaults of a new material: values from [Default] in the type of the member, tiling one and zero.
    HX_CHECK(At<f32>(block.defaults, 0) == 1.0f && At<f32>(block.defaults, 4) == 0.5f && At<f32>(block.defaults, 8) == 0.25f &&
             At<f32>(block.defaults, 12) == 1.0f);
    HX_CHECK(At<f32>(block.defaults, 16) == 1.0f && At<f32>(block.defaults, 20) == 1.0f && At<f32>(block.defaults, 24) == 0.0f &&
             At<f32>(block.defaults, 28) == 0.0f);
    HX_CHECK(At<f32>(block.defaults, 32) == 0.5f && At<i32>(block.defaults, 36) == 3 && At<u32>(block.defaults, 40) == 7u && At<u32>(block.defaults, 44) == 0u);

    // Setting a value writes it where the member lies and nowhere else.
    Array<u8> data = block.defaults;
    const f32 red[] = {1.0f, 0.0f, 0.0f, 0.5f}, count[] = {-4.0f};
    WritePropertyValue(shader.properties[tint], *b, red, data);
    WritePropertyValue(shader.properties[layers], *layout.Find(layers, PropertyTarget::Member), count, data);
    HX_CHECK(At<f32>(data, 4) == 0.0f && At<f32>(data, 12) == 0.5f && At<i32>(data, 36) == -4 && At<f32>(data, 32) == 0.5f && At<f32>(data, 16) == 1.0f);
}

HX_TEST(Material, ReportsMixedBlocksUnusedPropertiesAndWrongTypes)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    ShaderDescription shader;
    PassReflection pass;
    MaterialLayout layout;
    String error;

    // The overlay block has a member no property fills.
    HX_REQUIRE(Prepare(a, "Painted", "Overlay", shader, pass, error) && BindProperties(shader, pass, layout, error));
    HX_REQUIRE(layout.blocks.Size() == 1u && layout.bindings.Size() == 1u);
    HX_CHECK(layout.blocks[0].name == "overlay" && !layout.blocks[0].propertiesOnly && layout.unused.Size() == shader.properties.Size() - 1);

    // No property of Lit has the name of anything in its pass.
    HX_REQUIRE(Prepare(a, "Lit", "Forward", shader, pass, error) && BindProperties(shader, pass, layout, error));
    HX_CHECK(layout.blocks.Empty() && layout.bindings.Empty() && layout.unused.Size() == shader.properties.Size());

    HX_REQUIRE(Prepare(a, "BadMaterial", "Only", shader, pass, error));
    HX_CHECK(!BindProperties(shader, pass, layout, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("tint") && StringView(error).Contains("float2") && StringView(error).Contains("float4"));
}
