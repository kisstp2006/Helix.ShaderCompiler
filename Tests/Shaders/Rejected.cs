using System;
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

    [Storage(2)] public static RWTexture2D<float4> colors;
    [Storage(3, Format = StorageFormat.R32F)] public static RWTexture2D<uint> mismatched;

    // RGBA8 texels cannot be read back by Direct3D 11.
    [ComputeShader(1)]
    public static void ReadsColors([DispatchThreadId] uint3 id) => colors[new int2(0, 0)] = colors[new int2(1, 0)];

    [ComputeShader(1)]
    public static void WrongFormat([DispatchThreadId] uint3 id) => mismatched[new int2(0, 0)] = 1u;

    public struct HalfBlock
    {
        public half4 tint;
    }

    [Uniform(5)] public static HalfBlock halfBlock;

    [FragmentShader]
    [return: Target(0)]
    public static float4 HalfUniform() => (float4)halfBlock.tint;

    [System.Runtime.CompilerServices.InlineArray(3)]
    public struct Scalars
    {
        private float element;
    }

    public struct ScalarBlock
    {
        public Scalars values;
    }

    [Uniform(6)] public static ScalarBlock scalarBlock;

    [FragmentShader]
    [return: Target(0)]
    public static float4 ScalarArray() => new float4(scalarBlock.values[1]);

    public unsafe struct FixedWeights
    {
        public fixed float values[4];
    }

    [FragmentShader]
    [return: Target(0)]
    public static unsafe float4 FixedBuffer([Location(0)] float2 uv)
    {
        FixedWeights w = default;
        w.values[0] = uv.x;
        return new float4(w.values[0]);
    }

    private static float Gain { get; set; } = 0.5f;

    [FragmentShader]
    [return: Target(0)]
    public static float4 StaticAutoProperty([Location(0)] float2 uv) => new float4(uv * Gain, 0.0f, 1.0f);

    [Keyword(Dynamic = true)] public static readonly bool Sparkle;

    [Uniform(11)] public static HalfBlock onKeywordSlot;

    [FragmentShader]
    [return: Target(0)]
    public static float4 KeywordSlotTaken() => Sparkle ? new float4(1.0f) : (float4)onKeywordSlot.tint;

    [FragmentShader]
    [return: Target(0)]
    public static float4 PicksLambda([Location(0)] float2 uv)
    {
        Func<float, float> f = uv.x > 0.5f ? v => v * 2.0f : v => v * 0.5f;
        return new float4(f(uv.y));
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 NestedCapture([Location(0)] float2 uv)
    {
        float sum = 0.0f;
        for (int i = 0; i < 2; i++)
        {
            float k = i;
            Func<float, float> f = v => v * k + sum;
            sum = f(uv.x);
        }
        return new float4(sum);
    }

    [VertexShader]
    [return: Position]
    public static float4 HalfInput([Location(0)] half2 p) => new float4((float2)p, 0.0f, 1.0f);

    [FragmentShader]
    public static float4 NoTarget([Location(0)] float2 uv) => new float4(uv, 0.0f, 1.0f);
}
