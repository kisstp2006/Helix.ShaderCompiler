using Helix.ShaderIncludes;
using HelixEngine.Shaders;

namespace Helix.ShaderTests;

/// <summary>Small methods with a known IL shape for the assembly reader tests.</summary>
public static class Basics
{
    public static float Add(float a, float b) => a + b;

    public static int Loop(int n)
    {
        int sum = 0;
        for (int i = 0; i < n; i++)
            sum += i;
        return sum;
    }

    public static float Pick(int k)
    {
        switch (k)
        {
            case 0: return 1.0f;
            case 1: return 2.0f;
            case 2: return 4.0f;
            case 3: return 16.0f;
            default: return 8.0f;
        }
    }

    public static T Identity<T>(T value) where T : unmanaged => value;

    public static float3 Scale(float3 v, float s) => Identity(v) * s;

    public static void Outputs(float a, out float doubled, ref float3 accumulated)
    {
        doubled = a * 2.0f;
        accumulated += new float3(a);
    }

    public static double Wide(long a, double scale) => (a + 1234567890123L) * scale * 0.5;
}

/// <summary>A uniform block with a struct inside a struct.</summary>
public static class Nested
{
    public struct Light
    {
        public float4 color;
        public float2 range;
        public float2 pad;
    }

    public struct Scene
    {
        public float4 ambient;
        public Light key;
        public Light fill;
        public float exposure;
    }

    [Uniform(4)] public static Scene scene;

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv) => scene.ambient + scene.key.color * uv.x + scene.fill.color * scene.exposure;
}

/// <summary>A shader whose resources are declared by an included assembly.</summary>
public static class IncludedResources
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        SurfaceData s = new SurfaceData { albedo = new float3(uv, 1.0f), roughness = 0.5f };
        return new float4(Lighting.ShadeMain(s, new float3(0.0f, 0.0f, 1.0f)), 1.0f);
    }
}
