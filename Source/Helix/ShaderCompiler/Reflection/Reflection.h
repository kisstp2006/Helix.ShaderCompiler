#pragma once

#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    // What a compiled shader expects from the renderer: where the values of its uniform blocks lie, which resources it
    // binds at which slots, and what flows in and out of the stage. Read from a translated module, so it describes
    // one variant: resources and inputs that the variant does not use are not listed.

    // One value inside a uniform block. Members of nested structs are listed with a dotted name ("light.color").
    struct ReflectedMember
    {
        String name;
        Type type;
        // Bytes from the start of the block in std140, the layout of the block on every target (array elements on 16 bytes);
        // the C# struct may lie otherwise.
        u32 offset = 0;
        u32 size = 0;
    };

    struct ReflectedBlock
    {
        // Name of the C# field.
        String name;
        u32 slot = 0;
        // Bytes the bound buffer range must cover: the size of the struct rounded up to 16.
        u32 size = 0;
        Array<ReflectedMember> members;

        const ReflectedMember* Find(StringView memberName) const;
    };

    // A texture is sampled with the sampler bound at its slot, so samplers are not resources of their own.
    enum class ResourceKind : u8
    {
        Texture,
        StorageBuffer,
        StorageTexture,
    };

    struct ReflectedResource
    {
        String name;
        ResourceKind kind = ResourceKind::Texture;
        u32 slot = 0;
        // Storage resources: whether the shader writes it; buffers also give the element type and its size in bytes.
        bool writable = false;
        Type element;
        u32 stride = 0;
        StorageFormat format = StorageFormat::RGBA8;
        // A texture read with a comparison sampler; its slot needs one.
        bool comparison = false;
    };

    // A value entering or leaving the stage.
    struct ReflectedValue
    {
        String name;
        Semantic semantic = Semantic::None;
        // Location or target index.
        u32 index = 0;
        Type type;
        bool flat = false;
    };

    struct ShaderReflection
    {
        Stage stage = Stage::Vertex;
        Array<ReflectedBlock> blocks;
        Array<ReflectedResource> resources;
        // Vertex stage inputs are the vertex attributes: Location n is attribute n.
        Array<ReflectedValue> inputs, outputs;
        // Keywords the code read, with the values of this variant.
        Array<KeywordUse> keywords;
        u32 groupSize[3] = {1, 1, 1};
        // Whether the renderer has to bind the bindless tables (textures, buffers, the buffer heap as writable).
        BindlessUse bindless;

        const ReflectedBlock* FindBlock(StringView name) const;
        const ReflectedResource* FindResource(StringView name) const;
    };

    // Describes a module. Fails, like the backends, when a uniform struct or a buffer element does not lie the same
    // way in C# and on the GPU, or when the entry point has a value without a stage attribute.
    bool Reflect(const Module& module, ShaderReflection& out, String& error);

    // What the stages of one pass need together.
    struct PassReflection
    {
        Array<ReflectedBlock> blocks;
        Array<ReflectedResource> resources;
        Array<ReflectedValue> vertexInputs, targets;
        Array<KeywordUse> keywords;
        BindlessUse bindless;
    };

    // Joins a vertex and a fragment stage and checks that they fit: a slot holds the same block or resource in both,
    // and every value the fragment stage reads at a location is written by the vertex stage with the same type and
    // interpolation. Drivers report such mismatches late and vaguely; here the message names the value.
    bool LinkReflection(const ShaderReflection& vertex, const ShaderReflection& fragment, PassReflection& out, String& error);
}
