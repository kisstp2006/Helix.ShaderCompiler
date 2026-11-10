using HelixEngine.Shaders;
using static HelixEngine.Shaders.ShaderMath;

namespace Helix.ShaderTests;

public enum GlassDetail
{
    Low,
    Medium,
    High = 5,
}

/// <summary>A definition that uses every declarative feature.</summary>
[Shader("Tests/Glass", Fallback = "Tests/Lit", CustomEditor = "GlassEditor", LOD = 300)]
[Tag("Queue", "Transparent")]
[Tag("RenderType", "Transparent")]
public sealed class Glass : ShaderDefinition
{
    [Property("Tint"), MainColor, HDR, Default(1.0f, 0.5f, 0.25f, 0.5f)] public float4 tint;
    [Property("Base Map"), MainTexture, NoScaleOffset, DefaultTexture("white")] public Texture2D<float4> baseMap;
    [Property("Smoothness"), Range(0.0f, 1.0f), Default(0.5f)] public float smoothness;
    [Property("Layers"), HideInInspector] public int layers;
    [Property("Use Fog"), Toggle("Fog")] public float useFog;
    [Property("Detail"), KeywordEnum("Detail")] public float detail;
    [PerRendererData] public float2 offset;

    [Keyword(KeywordScope.Global)] public static readonly bool Fog;
    [Keyword(ShaderFeature = true)] public static readonly GlassDetail Detail;
    [Keyword(Dynamic = true)] public static readonly bool Flicker;

    public struct Varyings
    {
        [Position] public float4 position;
        [Location(0)] public float2 uv;
    }

    [Pass("Depth", LightMode = "DepthOnly", ColorMask = ColorWriteMask.None, OffsetFactor = 1.0f, OffsetUnits = 2.0f)]
    [Stencil(Reference = 3, WriteMask = 15, Compare = CompareFunction.Always, Pass = StencilOperation.Replace)]
    public static class Depth
    {
        [VertexShader]
        public static Varyings Vertex([Location(0)] float3 position, [Location(1)] float2 uv)
        {
            Varyings o;
            o.position = new float4(position, 1.0f);
            o.uv = uv;
            return o;
        }

        [FragmentShader]
        [return: Target(0)]
        public static float4 Fragment(Varyings input) => new float4(0.0f);
    }

    [Pass("Forward", LightMode = "ForwardBase", Cull = CullMode.Off, ZWrite = false, ZTest = CompareFunction.Less, ZClip = false,
          ColorMask = ColorWriteMask.Red | ColorWriteMask.Green | ColorWriteMask.Blue, AlphaToMask = true, Conservative = true)]
    [Tag("Stage", "Blended")]
    [Blend(BlendFactor.SrcAlpha, BlendFactor.OneMinusSrcAlpha)]
    [Blend(BlendFactor.One, BlendFactor.One, BlendFactor.Zero, BlendFactor.One, Target = 1, Operation = BlendOperation.Max,
           OperationAlpha = BlendOperation.Subtract)]
    [Stencil(Face = StencilFace.Front, ReadMask = 7, Compare = CompareFunction.Equal, Fail = StencilOperation.Zero)]
    [Stencil(Face = StencilFace.Back, Compare = CompareFunction.NotEqual, ZFail = StencilOperation.Invert)]
    public static class Forward
    {
        [VertexShader]
        public static Varyings Vertex([Location(0)] float3 position, [Location(1)] float2 uv) => Depth.Vertex(position, uv);

        [FragmentShader]
        [return: Target(0)]
        public static float4 Fragment(Varyings input)
        {
            float3 color = new float3(input.uv, 0.5f);
            if (Fog)
                color *= 0.5f;
            if (Detail == GlassDetail.High)
                color.z = 1.0f;
            if (Flicker)
                color.x = 1.0f - color.x;
            return new float4(color, 0.5f);
        }
    }
}

/// <summary>Two SubShaders: a detailed one whose passes take its render state, and a cheap one.</summary>
[Shader("Tests/Layered")]
public sealed class Layered : ShaderDefinition
{
    public static float4 Place(float2 p) => new float4(p, 0.0f, 1.0f);

    [SubShader(LOD = 400, Cull = CullMode.Front, ZTest = CompareFunction.Less)]
    [Tag("RenderType", "Opaque")]
    [Blend(BlendFactor.One, BlendFactor.One)]
    [Stencil(Reference = 2, Pass = StencilOperation.Replace)]
    public static class Detailed
    {
        [Pass("Forward", LightMode = "ForwardBase")]
        public static class Forward
        {
            [VertexShader]
            [return: Position]
            public static float4 Vertex([Location(0)] float2 p) => Place(p);

            [FragmentShader]
            [return: Target(0)]
            public static float4 Fragment() => new float4(1.0f);
        }

        [Pass("Outline", Cull = CullMode.Back, ZWrite = false)]
        [Blend(BlendFactor.SrcAlpha, BlendFactor.OneMinusSrcAlpha)]
        public static class Outline
        {
            [VertexShader]
            [return: Position]
            public static float4 Vertex([Location(0)] float2 p) => Place(p * 1.25f);

            [FragmentShader]
            [return: Target(0)]
            public static float4 Fragment() => new float4(0.0f, 0.0f, 0.0f, 1.0f);
        }
    }

    [SubShader(LOD = 100)]
    public static class Cheap
    {
        [Pass("Forward", Cull = CullMode.Off)]
        public static class Forward
        {
            [VertexShader]
            [return: Position]
            public static float4 Vertex([Location(0)] float2 p) => Place(p);

            [FragmentShader]
            [return: Target(0)]
            public static float4 Fragment() => new float4(0.5f);
        }
    }
}

// Definitions that cannot work; each is read on its own.
[Shader("Tests/BadMix")]
public sealed class BadMix : ShaderDefinition
{
    [SubShader]
    public static class Group
    {
    }

    [Pass("Loose")]
    public static class Loose
    {
        [VertexShader]
        [return: Position]
        public static float4 Vertex([Location(0)] float2 p) => new float4(p, 0.0f, 1.0f);

        [FragmentShader]
        [return: Target(0)]
        public static float4 Fragment() => new float4(1.0f);
    }
}

[Shader("Tests/BadProperty")]
public sealed class BadProperty : ShaderDefinition
{
    [Property("Label")] public string? label;
}

[Shader("Tests/BadPass")]
public sealed class BadPass : ShaderDefinition
{
    [Pass("Only")]
    public static class Only
    {
        [VertexShader]
        [return: Position]
        public static float4 Vertex([Location(0)] float2 p) => new float4(p, 0.0f, 1.0f);
    }
}

[Shader("Tests/BadToggle")]
public sealed class BadToggle : ShaderDefinition
{
    [Property("Fog"), Toggle("Missing")] public float fog;
}

/// <summary>A shader whose properties reach the code through a uniform block and textures of the same names.</summary>
[Shader("Tests/Painted")]
public sealed class Painted : ShaderDefinition
{
    [Property("Tint"), Default(1.0f, 0.5f, 0.25f, 1.0f)] public float4 tint;
    [Property("Base Map"), DefaultTexture("white")] public Texture2D<float4> baseMap;
    [Property("Detail Map"), NoScaleOffset] public Texture2D<float4> detailMap;
    [Property("Smoothness"), Range(0.0f, 1.0f), Default(0.5f)] public float smoothness;
    [Property("Layers"), Default(3.0f)] public int layers;
    [Property("Mask"), Default(7.0f)] public uint mask;
    [Property("Spare")] public float2 spare;

    public struct Frame
    {
        public float4x4 viewProjection;
    }

    public struct Material
    {
        public float4 tint;
        public float4 baseMap_ST;
        public float smoothness;
        public int layers;
        public uint mask;
    }

    public struct Overlay
    {
        public float4 tint;
        public float4 detailMap_ST;
    }

    public struct Varyings
    {
        [Position] public float4 position;
        [Location(0)] public float2 uv;
    }

    [Uniform(0)] public static Frame frame;
    [Uniform(1)] public static Material material;
    [Uniform(2)] public static Overlay overlay;

    [Pass("Forward")]
    public static class Forward
    {
        [Texture(0)] public static Texture2D<float4> baseMap;
        [Sampler(0)] public static SamplerState baseSampler;
        [Texture(3)] public static Texture2D<float4> detailMap;
        [Sampler(3)] public static SamplerState detailSampler;

        [VertexShader]
        public static Varyings Vertex([Location(0)] float3 position, [Location(1)] float2 uv)
        {
            Varyings o;
            o.position = Mul(frame.viewProjection, new float4(position, 1.0f));
            o.uv = uv;
            return o;
        }

        [FragmentShader]
        [return: Target(0)]
        public static float4 Fragment(Varyings input)
        {
            float4 color = baseMap.Sample(baseSampler, input.uv * material.baseMap_ST.xy + material.baseMap_ST.zw) * material.tint;
            color.xyz *= detailMap.Sample(detailSampler, input.uv).xyz * material.smoothness;
            if (material.layers > 2 && (material.mask & 1u) != 0u)
                color.w = 1.0f;
            return color;
        }
    }

    // The block of this pass has a member that is not a property: detailMap takes no scale and offset.
    [Pass("Overlay")]
    public static class Decal
    {
        [VertexShader]
        public static Varyings Vertex([Location(0)] float3 position, [Location(1)] float2 uv) => Forward.Vertex(position, uv);

        [FragmentShader]
        [return: Target(0)]
        public static float4 Fragment(Varyings input) => overlay.tint * overlay.detailMap_ST;
    }
}

[Shader("Tests/BadMaterial")]
public sealed class BadMaterial : ShaderDefinition
{
    [Property("Tint")] public float2 tint;

    public struct Material
    {
        public float4 tint;
    }

    [Uniform(0)] public static Material material;

    [Pass("Only")]
    public static class Only
    {
        [VertexShader]
        [return: Position]
        public static float4 Vertex([Location(0)] float2 p) => new float4(p, 0.0f, 1.0f);

        [FragmentShader]
        [return: Target(0)]
        public static float4 Fragment() => material.tint;
    }
}
