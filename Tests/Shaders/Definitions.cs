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
    [Property("Base Map"), MainTexture, NoScaleOffset, DefaultTexture("white")] public Texture2D baseMap;
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
            return new float4(color, 0.5f);
        }
    }
}

// Definitions that cannot work; each is read on its own.
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
