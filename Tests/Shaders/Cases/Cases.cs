using HelixEngine.Shaders;
using static HelixEngine.Shaders.ShaderMath;

// Every class here with a Fragment method is one execution test: the oracle runs it on the CPU for each pixel of a small
// image, the compiler turns it into GPU code, and the two images must match.
namespace Helix.ShaderTests.Cases;

/// <summary>The vertex shader of every case: a triangle that covers the target, uv (0, 0) at the top left.</summary>
public static class Fullscreen
{
    public struct Varyings
    {
        [Position] public float4 position;
        [Location(0)] public float2 uv;
    }

    [VertexShader]
    public static Varyings Vertex([Location(0)] float2 position)
    {
        Varyings o;
        o.position = new float4(position, 0.5f, 1.0f);
        o.uv = new float2(position.x * 0.5f + 0.5f, 0.5f - position.y * 0.5f);
        return o;
    }
}

public static class Gradient
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv) => new float4(uv, 0.25f, 1.0f);
}

public static class Arithmetic
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float3 a = new float3(uv, 0.5f);
        float3 b = a.zyx * 2.0f - 0.25f;
        float2 c = (a.xy + b.yz) / 3.0f;
        float4 r = new float4(c, a.z * b.x, 1.0f);
        r.xy = r.yx;
        r.z += 0.125f;
        r.w -= uv.x * 0.25f;
        r[1] = r[1] * 0.5f + -uv.y * 0.125f + 0.125f;
        return r * 0.75f + 0.1f;
    }
}

public static class Branches
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if (uv.x < 0.25f)
            r = 1.0f;
        else if (uv.x < 0.5f)
            g = 1.0f;
        else if (uv.x < 0.75f)
            b = 1.0f;
        else
        {
            r = 0.5f;
            g = 0.5f;
        }
        if (uv.y > 0.5f && uv.x > 0.3f || uv.y < 0.1f)
            b += 0.5f;
        float a = uv.y > 0.8f ? 0.25f : 1.0f;
        bool edge = uv.x > 0.9f || uv.y > 0.9f;
        if (!edge && uv.x < 0.05f)
            g = 0.75f;
        return new float4(r, g, b, a);
    }
}

public static class Loops
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float sum = 0.0f;
        for (int i = 0; i < 8; i++)
        {
            if (i == 3)
                continue;
            sum += uv.x * i;
            if (sum > 4.0f)
                break;
        }
        int n = 0;
        float t = uv.y;
        while (t < 1.0f)
        {
            t += 0.125f;
            n++;
        }
        float nested = 0.0f;
        for (int y = 0; y < 3; y++)
            for (int x = 0; x <= y; x++)
                nested += 1.0f;
        int k = 0;
        do
        {
            k += 2;
        } while (k < 5);
        return new float4(sum / 8.0f, n / 8.0f, nested / 12.0f, k / 8.0f);
    }
}

public static class Switches
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        int cell = (int)(uv.x * 7.0f);
        float3 c;
        switch (cell)
        {
            case 0:
                c = new float3(1.0f, 0.0f, 0.0f);
                break;
            case 1:
            case 2:
                c = new float3(0.0f, 1.0f, 0.0f);
                break;
            case 3:
                c = new float3(0.0f, 0.0f, 1.0f);
                break;
            case 5:
                c = new float3(1.0f, 1.0f, 0.0f);
                break;
            default:
                c = new float3(0.5f);
                break;
        }
        return new float4(c, uv.y);
    }
}

public static class Functions
{
    public struct Light
    {
        public float3 color;
        public float intensity;
    }

    private static float Square(float x) => x * x;

    private static void Split(float v, out float low, out float high)
    {
        low = Min(v, 0.5f);
        high = Max(v, 0.5f);
    }

    private static void Brighten(ref float3 color, float amount) => color += new float3(amount);

    private static Light MakeLight(float2 uv)
    {
        Light l;
        l.color = new float3(uv, 1.0f - uv.x);
        l.intensity = Square(uv.y) + 0.25f;
        return l;
    }

    private static T Pick<T>(bool first, T a, T b) where T : unmanaged => first ? a : b;

    private static float EarlyOut(float x)
    {
        if (x < 0.25f)
            return 0.0f;
        for (int i = 0; i < 4; i++)
        {
            if (x < 0.5f + i * 0.125f)
                return i * 0.25f;
        }
        return 1.0f;
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        Light l = MakeLight(uv);
        Split(uv.x, out float low, out float high);
        float3 c = l.color * l.intensity;
        Brighten(ref c, low * 0.25f);
        float2 p = Pick(uv.x > 0.5f, new float2(high, 0.0f), new float2(0.0f, high));
        return new float4(c.xy + p * 0.25f, c.z * 0.5f + EarlyOut(uv.y) * 0.5f, Pick(uv.y > 0.5f, 1.0f, 0.5f));
    }
}

public static class Intrinsics
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float x = uv.x, y = uv.y;
        float a = Sin(x * 6.2831853f) * 0.5f + 0.5f;
        float b = Sqrt(y) * Pow(x + 0.5f, 2.0f) * 0.4f;
        float3 v = Normalize(new float3(x - 0.5f, y - 0.5f, 0.5f));
        float d = Saturate(Dot(v, new float3(0.0f, 0.0f, 1.0f)));
        float c = Smoothstep(0.2f, 0.8f, x) * Step(0.5f, y) + Frac(x * 4.0f) * 0.25f;
        float e = Lerp(Abs(x - 0.5f), Floor(y * 4.0f) / 4.0f, 0.5f) + Length(new float2(x, y)) * 0.1f + Atan2(y, x + 0.1f) * 0.1f;
        float3 w = Cross(v, new float3(0.0f, 1.0f, 0.0f)) * 0.5f + Reflect(v, new float3(0.0f, 0.0f, 1.0f)) * 0.25f;
        float f = Fmod(x * 5.0f, 1.5f) * 0.25f + Exp(-y) * 0.25f + Cos(y * 3.0f) * 0.125f + Max(w.x, 0.0f) * 0.25f + Min(w.z + 0.5f, 0.25f);
        return new float4(a, Clamp(b, 0.0f, 1.0f), c * 0.5f + d * 0.25f, Saturate(e * 0.5f + f * 0.5f));
    }
}

public static class Integers
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        int ix = (int)(uv.x * 16.0f), iy = (int)(uv.y * 16.0f);
        uint h = (uint)(ix * 73 + iy * 151);
        h ^= h >> 3;
        h = h * 2654435761u;
        h ^= h << 5;
        uint bits = (h >> 8) & 255u;
        int m = (ix * 7 + iy * 3) % 5;
        int s = (ix << 2) | (iy & 3);
        int third = (20 - ix) / 3;
        uint2 pair = new uint2((uint)ix, (uint)iy) * 3u + 1u;
        return new float4(bits / 255.0f, m / 4.0f, (s & 63) / 63.0f, (third + (int)(pair.x ^ pair.y) % 4) / 12.0f);
    }
}

public static class Matrices
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float4x4 m = new float4x4(new float4(0.5f, 0.0f, 0.0f, 0.0f), new float4(0.0f, 0.5f, 0.0f, 0.0f), new float4(0.0f, 0.0f, 1.0f, 0.0f),
                                  new float4(0.25f, 0.125f, 0.0f, 1.0f));
        float4 p = Mul(m, new float4(uv, 0.0f, 1.0f));
        float3x3 r = new float3x3(new float3(0.0f, 1.0f, 0.0f), new float3(-1.0f, 0.0f, 0.0f), new float3(0.0f, 0.0f, 1.0f));
        float3 q = Mul(r, new float3(uv.x, uv.y, 1.0f)) * 0.5f + 0.5f;
        float4x4 t = Transpose(m);
        float2x2 s = new float2x2(new float2(uv.x, 0.0f), new float2(0.0f, uv.y));
        float2 d = Mul(Mul(s, s), new float2(1.0f, 1.0f));
        return new float4(p.x + q.x * 0.25f, p.y + t.c0.w, q.y * d.x, d.y * 0.5f + m.c3.y);
    }
}

public static class Uniforms
{
    public struct Params
    {
        public float4x4 transform;
        public float4 tint;
        public float2 offset;
        public float scale;
        public uint mode;
        public int3 steps;
        public float bias;
    }

    [Uniform(0)] public static Params data;

    public static void Setup()
    {
        data.transform = new float4x4(new float4(0.5f, 0.0f, 0.0f, 0.0f), new float4(0.0f, 0.75f, 0.0f, 0.0f), new float4(0.0f, 0.0f, 1.0f, 0.0f),
                                      new float4(0.125f, 0.0f, 0.0f, 1.0f));
        data.tint = new float4(1.0f, 0.5f, 0.75f, 1.0f);
        data.offset = new float2(0.25f, 0.125f);
        data.scale = 0.75f;
        data.mode = 2u;
        data.steps = new int3(1, 3, 5);
        data.bias = 0.0625f;
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float4 p = Mul(data.transform, new float4(uv + data.offset, 0.0f, 1.0f)) * data.scale;
        float mode = data.mode == 2u ? 1.0f : 0.0f;
        float steps = (data.steps.x + data.steps.y * 2 + data.steps.z) / 16.0f;
        return new float4(p.xy * data.tint.xy + data.bias, mode * data.tint.z, steps);
    }
}

public static class Textures
{
    [Texture(0)] public static Texture2D image;
    [Sampler(0)] public static SamplerState imageSampler;
    [Texture(1)] public static Texture2D smooth;
    [Sampler(1)] public static SamplerState smoothSampler;

    public static void Setup()
    {
        // Channel values are multiples of 1/255, so the 8 bit GPU texture holds exactly what the CPU reads.
        float4[] pixels = new float4[16];
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                pixels[y * 4 + x] = new float4(x * 85 / 255.0f, y * 85 / 255.0f, (x ^ y) & 1, 1.0f);
        image = Texture2D.FromPixels(pixels, 4, 4);
        imageSampler = new SamplerState { Filter = SamplerFilter.Nearest, Address = SamplerAddress.Repeat };
        smooth = Texture2D.FromPixels(new float4[] { new(1, 0, 0, 1), new(0, 1, 0, 1), new(0, 0, 1, 1), new(1, 1, 1, 1) }, 2, 2);
        smoothSampler = new SamplerState { Filter = SamplerFilter.Linear, Address = SamplerAddress.Clamp };
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float4 a = image.Sample(imageSampler, uv * 2.0f);
        float4 b = smooth.Sample(smoothSampler, uv);
        float4 c = image.Load(new int2(1, 2), 0);
        int2 size = image.Size;
        return new float4(a.xy * 0.5f + b.xy * 0.5f, b.z * 0.5f + c.x * 0.5f, size.x / 8.0f + a.z * 0.25f);
    }
}

public static class Discards
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        int cx = (int)(uv.x * 8.0f), cy = (int)(uv.y * 8.0f);
        if (((cx + cy) & 1) == 0)
            Discard();
        return new float4(uv.y, uv.x, 1.0f, 1.0f);
    }
}

public static class BoolVectors
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float3 v = new float3(uv, uv.x * uv.y);
        bool3 big = v > new float3(0.5f);
        float3 a = Select(big, new float3(1.0f, 0.5f, 0.25f), v);
        bool2 equal = new int2((int)(uv.x * 4.0f), 1) == new int2(1, (int)(uv.y * 2.0f));
        float any = Any(equal) ? 1.0f : 0.0f;
        float all = All(big.xy) ? 0.5f : 0.0f;
        int3 picked = Select(!big, new int3(1, 2, 3), new int3(0));
        bool3 both = big & new bool3(uv.x > 0.25f, true, uv.y < 0.75f);
        float extra = Any(both) ? 0.125f : 0.0f;
        return new float4(a.xy * 0.5f + new float2(any, all) * 0.25f, a.z + extra, (picked.x + picked.y + picked.z) / 6.0f);
    }
}

public static class StructLocals
{
    public struct Inner
    {
        public float2 a;
        public float b;
    }

    public struct Outer
    {
        public Inner inner;
        public float3 c;
        public int count;
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        Outer o;
        o.inner.a = uv;
        o.inner.b = uv.x * uv.y;
        o.c = new float3(0.25f);
        o.count = 3;
        Outer copy = o;
        copy.inner.a.x += 0.25f;
        copy.c.yz = o.inner.a;
        copy.count += o.count;
        Inner inner = copy.inner;
        inner.b *= 2.0f;
        Inner cleared = default;
        cleared.b = inner.b * 0.5f;
        return new float4(copy.inner.a.x * 0.5f, copy.c.y, cleared.b + copy.c.x, copy.count / 8.0f);
    }
}
