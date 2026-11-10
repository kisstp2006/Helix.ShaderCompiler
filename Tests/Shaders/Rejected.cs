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

    [Storage(0)] public static RWStructuredBuffer<float3> points;
    [Storage(1)] public static StructuredBuffer<uint> fixedValues;

    // A float3 element takes 12 bytes in C# and 16 in a GPU buffer.
    [ComputeShader(1)]
    public static void PaddedElements([DispatchThreadId] uint3 id) => points[(int)id.x] = new float3(1.0f);

    [ComputeShader(1)]
    public static void CopiesBuffer([DispatchThreadId] uint3 id)
    {
        StructuredBuffer<uint> copy = fixedValues;
        points[(int)id.x] = new float3(copy[0]);
    }

    [FragmentShader]
    public static float4 NoTarget([Location(0)] float2 uv) => new float4(uv, 0.0f, 1.0f);
}
