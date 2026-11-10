#pragma once

#include <Helix/ShaderCompiler/Assembly/Assembly.h>
#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    // What a shader assembly declares around its code: the shaders, their material properties, keywords and passes with
    // render state. Read from the attributes of classes derived from ShaderDefinition; no code is translated here.
    // The numeric values of the enums are those of the C# enums of the shader library.

    struct ShaderTag
    {
        String name, value;
    };

    enum class PropertyFlags : u32
    {
        None = 0,
        Hdr = 1 << 0,
        NoScaleOffset = 1 << 1,
        MainTexture = 1 << 2,
        MainColor = 1 << 3,
        PerRendererData = 1 << 4,
        HideInInspector = 1 << 5,
        Toggle = 1 << 6,
        KeywordEnum = 1 << 7,
    };
    constexpr u32 operator|(PropertyFlags a, PropertyFlags b) { return u32(a) | u32(b); }

    struct ShaderProperty
    {
        // Field name in C#, the name materials use.
        String name;
        String displayName;
        // Float, int, uint, bool scalars and vectors, matrices, or a texture.
        Type type;
        u32 flags = 0;
        bool hasRange = false;
        f32 rangeMin = 0, rangeMax = 0;
        f32 defaultValue[4] = {};
        // Built in texture name for texture properties; empty when none was given.
        String defaultTexture;
        // Keyword driven by a Toggle or KeywordEnum property; empty for a Toggle without a keyword.
        String keyword;

        bool Has(PropertyFlags flag) const { return (flags & u32(flag)) != 0; }
    };

    struct ShaderKeyword
    {
        String name;
        bool global = false;
        // Variants of a shader feature are only built for the values materials use.
        bool shaderFeature = false;
        bool dynamic = false;
        // Member names of an enum keyword in value order with their values; empty for a bool keyword.
        Array<String> valueNames;
        Array<i64> values;
    };

    enum class CullMode : u8 { Off, Front, Back };
    enum class CompareFunction : u8 { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
    enum class BlendFactor : u8
    {
        Zero, One, SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha,
        SrcAlphaSaturate,
    };
    enum class BlendOperation : u8 { Add, Subtract, ReverseSubtract, Min, Max };
    enum class StencilOperation : u8 { Keep, Zero, Replace, IncrementSaturate, DecrementSaturate, Invert, IncrementWrap, DecrementWrap };

    struct BlendState
    {
        // -1: every target.
        i32 target = -1;
        BlendFactor source = BlendFactor::One, destination = BlendFactor::Zero;
        BlendFactor sourceAlpha = BlendFactor::One, destinationAlpha = BlendFactor::Zero;
        BlendOperation operation = BlendOperation::Add, operationAlpha = BlendOperation::Add;
    };

    struct StencilFaceState
    {
        CompareFunction compare = CompareFunction::Always;
        StencilOperation pass = StencilOperation::Keep, fail = StencilOperation::Keep, zFail = StencilOperation::Keep;
    };

    struct RenderState
    {
        CullMode cull = CullMode::Back;
        bool zWrite = true;
        CompareFunction zTest = CompareFunction::LessEqual;
        bool zClip = true;
        f32 offsetFactor = 0, offsetUnits = 0;
        // Bit 0 red, 1 green, 2 blue, 3 alpha.
        u8 colorMask = 15;
        bool alphaToMask = false;
        bool conservative = false;
        // Empty: blending is off.
        Array<BlendState> blend;
        bool stencil = false;
        u8 stencilReference = 0, stencilReadMask = 255, stencilWriteMask = 255;
        StencilFaceState stencilFront, stencilBack;
    };

    struct ShaderPass
    {
        String name, lightMode;
        Array<ShaderTag> tags;
        RenderState state;
        // TypeDef row of the pass class and MethodDef rows of its entry points (0 when the pass has none).
        u32 type = 0, vertex = 0, fragment = 0;
        // Index of its SubShader in ShaderDescription::subShaders.
        u32 subShader = 0;
    };

    // A group of passes; the renderer takes the first SubShader whose LOD is within its limit.
    struct ShaderSubShader
    {
        i32 lod = 0;
        Array<ShaderTag> tags;
        // TypeDef row of the SubShader class; 0 for the one the passes of a definition without SubShader classes form.
        u32 type = 0;
    };

    struct ShaderDescription
    {
        String name, fallback, customEditor;
        i32 lod = 0;
        Array<ShaderTag> tags;
        Array<ShaderProperty> properties;
        Array<ShaderKeyword> keywords;
        // At least one. Without SubShader classes the passes form one, with the LOD and tags of the shader.
        Array<ShaderSubShader> subShaders;
        // In the order the pass classes are written, SubShader after SubShader.
        Array<ShaderPass> passes;
        u32 type = 0;

        // The first pass of that name: in the first SubShader that has one.
        const ShaderPass* FindPass(StringView passName) const;
        const ShaderProperty* FindProperty(StringView propertyName) const;
        const ShaderKeyword* FindKeyword(StringView keywordName) const;
    };

    // Reads every shader definition of the assembly. Fails with a message naming the shader and the member for
    // declarations that cannot work: a property type materials cannot hold, a keyword that is neither bool nor enum,
    // a pass without a vertex or fragment shader, two passes of one SubShader or two properties with one name, passes
    // next to SubShader classes.
    bool ReadShaderDefinitions(const Assembly& assembly, Array<ShaderDescription>& out, String& error);

    // The definition of one class (a TypeDef row with the Shader attribute).
    bool ReadShaderDefinition(const Assembly& assembly, u32 typeRow, ShaderDescription& out, String& error);
}
