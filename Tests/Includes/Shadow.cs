using HelixEngine.Shaders;

namespace Helix.ShaderIncludes;

/// <summary>A shader of the include assembly whose pass other shaders take with UsePass.</summary>
[Shader("Includes/Shadow")]
public sealed class Shadow : ShaderDefinition
{
    [Keyword] public static readonly bool Soft;

    [Pass("ShadowCaster", LightMode = "ShadowCaster", ColorMask = ColorWriteMask.None)]
    public static class ShadowCaster
    {
        [VertexShader]
        [return: Position]
        public static float4 Vertex([Location(0)] float3 position) => new float4(position, 1.0f);

        [FragmentShader]
        [return: Target(0)]
        public static float4 Fragment() => Soft ? new float4(0.5f) : new float4(0.0f);
    }
}
