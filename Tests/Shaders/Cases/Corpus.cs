using HelixEngine.Shaders;
using static HelixEngine.Shaders.ShaderMath;

// More execution cases, each about one area of the language or the library.
namespace Helix.ShaderTests.Cases;

/// <summary>Swizzles read and written, also in compound assignments and on the left of a call result.</summary>
public static class Swizzles
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float4 v = new float4(uv, uv.yx * 0.5f);
        v.zw = v.xy * 0.25f;
        v.xz += new float2(0.125f, 0.0625f);
        float3 w = v.wzy;
        w.yx = w.xy;
        return new float4(w, v.x * 0.5f).yzxw;
    }
}

/// <summary>Integer arithmetic: shifts, masks, signed division and remainder, unsigned wrap around.</summary>
public static class IntegerBits
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        int x = (int)(uv.x * 16.0f), y = (int)(uv.y * 16.0f);
        int a = (x << 2) | (y & 3);
        int b = (a ^ 0x2A) >> 1;
        int c = (x - 8) / 3;
        int d = (y - 8) % 3;
        uint e = (uint)x * 0x9E3779B9u;
        uint f = e >> 28;
        return new float4((a & 63) / 63.0f, (b & 31) / 31.0f, (c + 3) / 6.0f + d * 0.0625f, f / 15.0f);
    }
}

/// <summary>Vector comparisons with Any, All and Select.</summary>
public static class Comparisons
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float3 v = new float3(uv, uv.x * uv.y);
        bool3 big = v > new float3(0.5f);
        bool3 small = v < new float3(0.25f);
        float r = Any(big) ? 1.0f : 0.0f;
        float g = All(small) ? 1.0f : 0.25f;
        float b = Select(uv.x < uv.y, 0.75f, 0.125f);
        return new float4(r, g, b, Any(big & !small) ? 0.5f : 1.0f);
    }
}

/// <summary>Nested branches with short circuit conditions.</summary>
public static class NestedBranches
{
    private static bool Inside(float2 p, float r) => Dot(p, p) < r * r;

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float2 p = uv - 0.5f;
        float4 c = new float4(0.0f, 0.0f, 0.0f, 1.0f);
        if (Inside(p, 0.4f) && !Inside(p, 0.2f))
        {
            if (p.x > 0.0f || p.y > 0.25f)
                c.x = 1.0f;
            else if (p.y < -0.1f && p.x < -0.1f)
                c.y = 1.0f;
            else
                c.z = 0.5f;
        }
        else if (uv.x > 0.9f || (uv.y > 0.9f && uv.x < 0.5f))
            c.xyz = new float3(0.25f);
        return c;
    }
}

/// <summary>While and do loops with breaks on data.</summary>
public static class WhileLoops
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float v = uv.x;
        int steps = 0;
        while (v < 2.0f)
        {
            v = v * 1.5f + 0.1f;
            steps++;
            if (steps > 12)
                break;
        }
        int n = 0;
        float w = uv.y;
        do
        {
            w -= 0.125f;
            n++;
        } while (w > 0.0f);
        return new float4(steps / 12.0f, n / 9.0f, Frac(v), 1.0f);
    }
}

public enum Shade
{
    None,
    Red,
    Green,
    Blue,
    White,
    Gray,
}

/// <summary>Switches on an int with many cases and on an enum.</summary>
public static class ManyCases
{
    private static float3 Color(Shade s) => s switch
    {
        Shade.Red => new float3(1.0f, 0.0f, 0.0f),
        Shade.Green => new float3(0.0f, 1.0f, 0.0f),
        Shade.Blue => new float3(0.0f, 0.0f, 1.0f),
        Shade.White => new float3(1.0f),
        Shade.Gray => new float3(0.5f),
        _ => new float3(0.0f),
    };

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        int cell = (int)(uv.x * 4.0f) + 4 * (int)(uv.y * 4.0f);
        float a;
        switch (cell)
        {
            case 0: case 5: a = 0.1f; break;
            case 1: a = 0.2f; break;
            case 2: case 7: case 9: a = 0.3f; break;
            case 3: a = 0.4f; break;
            case 10: a = 0.6f; break;
            case 15: a = 1.0f; break;
            default: a = 0.5f; break;
        }
        return new float4(Color((Shade)(cell % 6)) * 0.75f, a);
    }
}

/// <summary>Transcendental functions over ranges where every implementation agrees to well within the tolerance.</summary>
public static class MathFunctions
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float s = Sin(uv.x * 6.0f) * 0.5f + 0.5f;
        float c = Cos(uv.y * 4.0f) * 0.25f + 0.5f;
        float e = Exp(-uv.x * 2.0f) * 0.5f + Log(1.0f + uv.y) * 0.5f;
        float p = Pow(uv.x + 0.1f, 2.5f) * 0.5f + Sqrt(uv.y) * 0.25f + Rsqrt(1.0f + uv.x) * 0.25f;
        float t = Atan2(uv.y - 0.5f, uv.x - 0.5f) / 6.2831853f + 0.5f;
        return new float4(s, c * 0.5f + t * 0.5f, e, p);
    }
}

/// <summary>Geometric functions: dot, cross, length, distance, normalize, reflect and refract.</summary>
public static class Geometry
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float3 n = Normalize(new float3(uv.x - 0.5f, uv.y - 0.5f, 1.0f));
        float3 l = Normalize(new float3(0.3f, 0.5f, 0.8f));
        float3 r = Reflect(-l, n);
        float3 t = Refract(-l, n, 0.75f);
        float3 x = Cross(n, l);
        return new float4(Saturate(Dot(n, l)), r.z * 0.5f + 0.5f, t.z * -0.5f + 0.5f,
                          Saturate(Length(x) + Distance(uv, new float2(0.5f)) * 0.5f));
    }
}

/// <summary>Clamping and blending functions.</summary>
public static class Clamping
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float a = Saturate(uv.x * 2.0f - 0.5f);
        float b = Clamp(uv.y * 3.0f - 1.0f, 0.2f, 0.8f);
        float c = Step(0.5f, uv.x) * 0.5f + Smoothstep(0.2f, 0.8f, uv.y) * 0.5f;
        float d = Lerp(0.25f, 0.75f, Frac(uv.x * 3.0f)) + Fmod(uv.y * 5.0f, 1.0f) * 0.25f;
        return new float4(a, b, c, Saturate(d));
    }
}

/// <summary>Matrices of the three sizes: construction, columns, transpose and products.</summary>
public static class MatrixMath
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float2x2 r = new float2x2(new float2(0.0f, 1.0f), new float2(-1.0f, 0.0f));
        float2 a = Mul(r, uv - 0.5f) + 0.5f;
        float3x3 s = new float3x3(new float3(0.5f, 0.0f, 0.0f), new float3(0.0f, 0.25f, 0.0f), new float3(0.25f, 0.25f, 1.0f));
        float3 b = Mul(Transpose(s), new float3(uv, 1.0f));
        float4x4 m = new float4x4(new float4(1.0f, 0.0f, 0.0f, 0.0f), new float4(0.0f, 1.0f, 0.0f, 0.0f), new float4(0.0f, 0.0f, 1.0f, 0.0f),
                                  new float4(0.25f, -0.25f, 0.0f, 1.0f));
        float4 c = Mul(Mul(m, m), new float4(uv * 0.5f, 0.0f, 1.0f));
        return new float4(a.x, Saturate(b.x), Saturate(c.x), Saturate(c.y + 0.5f) * 0.5f + m[3].x);
    }
}

public struct Hit
{
    public float distance;
    public float3 normal;
}

/// <summary>Functions returning structs, out and ref parameters.</summary>
public static class StructReturns
{
    private static Hit Trace(float2 p, out bool found)
    {
        Hit h;
        h.distance = Length(p - 0.5f);
        h.normal = new float3(p - 0.5f, 0.25f);
        found = h.distance < 0.4f;
        return h;
    }

    private static void Shade(ref float4 color, in Hit hit)
    {
        color.xyz = Normalize(hit.normal) * 0.5f + 0.5f;
        color.w = 1.0f - hit.distance;
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        Hit h = Trace(uv, out bool found);
        float4 c = new float4(0.0f, 0.0f, 0.0f, 1.0f);
        if (found)
            Shade(ref c, h);
        return c;
    }
}

/// <summary>A chain of helpers five calls deep.</summary>
public static class CallChains
{
    private static float A(float x) => x * 0.5f + 0.1f;
    private static float B(float x) => A(x) + A(x * 0.5f);
    private static float C(float x, float y) => B(x) * 0.5f + B(y) * 0.25f;
    private static float2 D(float2 p) => new float2(C(p.x, p.y), C(p.y, p.x));
    private static float4 E(float2 p) => new float4(D(p), D(p.yx).x, 1.0f);

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv) => E(uv);
}

/// <summary>Generic helpers instantiated with several types.</summary>
public static class Generics
{
    private static T Pick<T>(bool first, T a, T b) => first ? a : b;

    private static void Swap<T>(ref T a, ref T b)
    {
        T t = a;
        a = b;
        b = t;
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float2 a = uv, b = uv.yx * 0.5f;
        if (uv.x > uv.y)
            Swap(ref a, ref b);
        int i = Pick(uv.x > 0.5f, 3, 1);
        float3 c = Pick(uv.y > 0.5f, new float3(0.25f), new float3(0.75f));
        return new float4(a.x, b.y, c.x, i * 0.25f);
    }
}

/// <summary>Conversions between float, int and uint, with truncation toward zero.</summary>
public static class Conversions
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float f = (uv.x - 0.5f) * 10.0f;
        int i = (int)f;
        uint u = (uint)(uv.y * 200.0f);
        float back = i;
        int2 v = (int2)(uv * 7.0f);
        uint3 w = new uint3(u, u / 3u, u % 7u);
        return new float4((back + 5.0f) / 10.0f, (u & 255u) / 255.0f, (v.x + v.y) / 14.0f, (w.y + w.z) / 80.0f);
    }
}

/// <summary>Constants: const fields, folded expressions and static readonly values of the shader.</summary>
public static class Constants
{
    private const float Half = 0.5f;
    private const int Steps = 4;
    private const float Quarter = Half * Half;

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float sum = 0.0f;
        for (int i = 0; i < Steps; i++)
            sum += uv.x * Quarter;
        return new float4(sum, uv.y * Half + Quarter, (Steps - 1) / 3.0f * 0.5f, 1.0f);
    }
}

/// <summary>A discard inside a loop and after a call.</summary>
public static class DiscardInLoop
{
    private static float Pattern(float2 p) => Frac(p.x * 3.0f) + Frac(p.y * 3.0f);

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float acc = 0.0f;
        for (int i = 0; i < 3; i++)
        {
            acc += Pattern(uv + i * 0.1f);
            if (acc > 4.5f)
                Discard();
        }
        if (Pattern(uv) < 0.2f)
            Discard();
        return new float4(acc / 6.0f, uv.x, uv.y, 1.0f);
    }
}

/// <summary>Half precision vectors with swizzles, mixed with float.</summary>
public static class HalfVectors
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        half4 h = new half4((half)uv.x, (half)uv.y, (half)0.25f, (half)1.0f);
        h.xy = h.yx * (half)0.5f;
        half3 g = h.zyx + new half3((half)0.125f);
        float f = (float)g.x * uv.x + (float)h.w * 0.25f;
        return new float4((float3)g, f);
    }
}

/// <summary>Conditional expressions nested and chained.</summary>
public static class Ternaries
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float a = uv.x < 0.25f ? 0.1f : uv.x < 0.5f ? 0.3f : uv.x < 0.75f ? 0.6f : 0.9f;
        float b = uv.y > 0.5f ? (uv.x > 0.5f ? 1.0f : 0.5f) : (uv.x > 0.5f ? 0.25f : 0.0f);
        int c = uv.x + uv.y > 1.0f ? 3 : 1;
        return new float4(a, b, c * 0.25f, a > b ? 1.0f : 0.5f);
    }
}

/// <summary>Bit casts between float, int and uint.</summary>
public static class BitCasts
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        uint bits = AsUInt(uv.x + 1.0f);
        float back = AsFloat(bits);
        int signBits = AsInt(-uv.y);
        uint mantissa = bits & 0x7FFFFFu;
        return new float4(back - 1.0f, (mantissa >> 15) / 255.0f, signBits < 0 ? 1.0f : 0.0f, ((bits >> 23) & 255u) / 255.0f);
    }
}
