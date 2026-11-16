#pragma once

#include <Helix/ShaderCompiler/Definition/ShaderDefinition.h>
#include <Helix/ShaderCompiler/Reflection/Reflection.h>

namespace helix::shadercompiler
{
    // Where the material properties of a shader go in one compiled pass. A property is tied to shader code by its name:
    // a number, vector or matrix property `tint` is the member `tint` of a uniform block of the pass, a texture property
    // `baseMap` is the texture `baseMap`, and its tiling goes to the float4 member `baseMap_ST` (scale xy, offset zw)
    // when the pass has one and the property is not marked NoScaleOffset.

    enum class PropertyTarget : u8
    {
        // Bytes inside a uniform block.
        Member,
        // A texture slot.
        Texture,
        // The float4 scale and offset of a texture property inside a uniform block.
        ScaleOffset,
    };

    struct PropertyBinding
    {
        // Index into ShaderDescription::properties.
        u32 property = 0;
        PropertyTarget target = PropertyTarget::Member;
        // Member and ScaleOffset: index into MaterialLayout::blocks, and where the value lies in the block.
        u32 block = 0, offset = 0, size = 0;
        // Texture: the slot, and whether it takes cube textures.
        u32 slot = 0;
        bool cube = false;
    };

    // A uniform block that takes at least one property.
    struct MaterialBlock
    {
        String name;
        u32 slot = 0;
        // The block as a new material fills it: zero, with the defaults of the properties written in.
        Array<u8> defaults;
        // False when the block also has members that are no properties; the renderer has to fill those.
        bool propertiesOnly = true;
    };

    // A texture of a pass that no property names: bound by its name (values by name, see NamedValues).
    struct NamedTexture
    {
        String name;
        u32 slot = 0;
        bool cube = false;
    };

    struct MaterialLayout
    {
        Array<PropertyBinding> bindings;
        Array<MaterialBlock> blocks;
        // With NamedValues: the textures without a property.
        Array<NamedTexture> namedTextures;
        // Properties this pass (in this variant) does not use, as indices into ShaderDescription::properties.
        Array<u32> unused;

        // First binding of a property with the given target; null when there is none.
        const PropertyBinding* Find(u32 property, PropertyTarget target) const;
    };

    // Values by name (the reference's globals and property blocks): every uniform block of the pass outside the
    // reserved slots (the renderer's own blocks) is a material block, its members without a property filled by name, and
    // the textures without a property are named texture slots.
    struct NamedValues
    {
        Span<const u32> reservedSlots;
    };

    // Ties the properties of a shader to what one pass exposes. Fails when a property and the member or texture of its
    // name have different types, naming both. Without named values only the blocks that take a property are material blocks.
    bool BindProperties(const ShaderDescription& shader, const PassReflection& pass, MaterialLayout& out, String& error, const NamedValues* named = nullptr);

    // Writes a property value into block data laid out like MaterialBlock::defaults: up to 16 numbers, converted to
    // the type of the member (float, int, uint; a bool member takes 0 or 1 as a 32 bit integer).
    void WritePropertyValue(const ShaderProperty& property, const PropertyBinding& binding, Span<const f32> value, Array<u8>& blockData);
}
