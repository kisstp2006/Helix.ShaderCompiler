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
