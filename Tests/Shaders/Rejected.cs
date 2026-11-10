using HelixEngine.Shaders;

namespace Helix.ShaderTests;

/// <summary>Shaders the compiler must refuse with a clear message.</summary>
public static class Rejected
{
    public struct Misaligned
    {
        public float weight;
        public float3 direction;
    }

    [Uniform(0)] public static Misaligned settings;

    private static int Depth(int n) => n <= 0 ? 0 : Depth(n - 1) + 1;

    [FragmentShader]
    [return: Target(0)]
    public static float4 Recursive([Location(0)] float2 uv) => new float4(Depth(3) * uv.x);

    [FragmentShader]
    [return: Target(0)]
    public static float4 Layout([Location(0)] float2 uv) => new float4(settings.direction * settings.weight, uv.x);

    [FragmentShader]
    public static float4 NoTarget([Location(0)] float2 uv) => new float4(uv, 0.0f, 1.0f);
}
