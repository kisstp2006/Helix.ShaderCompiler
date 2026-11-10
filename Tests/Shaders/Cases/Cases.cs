using System;
using Helix.ShaderIncludes;
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

    /// <summary>A constant color at the depth of the fullscreen triangle, drawn after cases that write depth.</summary>
    [FragmentShader]
    [return: Target(0)]
    public static float4 Probe() => new float4(1.0f, 0.0f, 1.0f, 1.0f);

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

public static class Continues
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float sum = 0.0f;
        int i = 0;
        while (i < 8)
        {
            i++;
            if ((i & 1) == 0)
                continue;
            sum += uv.x;
            for (int j = 0; j < 3; j++)
            {
                if (j == 1 && uv.y > 0.5f)
                    continue;
                sum += 0.125f;
            }
            if (sum > 2.0f)
                continue;
            sum += 0.25f;
        }
        return new float4(sum / 8.0f, i / 8.0f, 0.0f, 1.0f);
    }
}

public static class EarlyReturns
{
    static int FirstAbove(float limit, float step)
    {
        float t = 0.0f;
        for (int i = 0; i < 16; i++)
        {
            for (int j = 0; j < 2; j++)
            {
                t += step;
                if (t > limit)
                    return i;
            }
        }
        return 16;
    }

    static float Pick(float2 uv)
    {
        if (uv.x < 0.25f)
            return 0.1f;
        if (uv.y < 0.5f)
            return uv.x;
        return uv.y * 0.5f;
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        int a = FirstAbove(uv.x * 2.0f, 0.1f);
        int b = FirstAbove(0.5f, uv.y * 0.2f + 0.01f);
        return new float4(a / 16.0f, b / 16.0f, Pick(uv), 1.0f);
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
    [Texture(0)] public static Texture2D<float4> image;
    [Sampler(0)] public static SamplerState imageSampler;
    [Texture(1)] public static Texture2D<float4> smooth;
    [Sampler(1)] public static SamplerState smoothSampler;

    public static void Setup()
    {
        // Channel values are multiples of 1/255, so the 8 bit GPU texture holds exactly what the CPU reads.
        float4[] pixels = new float4[16];
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                pixels[y * 4 + x] = new float4(x * 85 / 255.0f, y * 85 / 255.0f, (x ^ y) & 1, 1.0f);
        image = Texture2D<float4>.FromTexels(pixels, 4, 4);
        imageSampler = new SamplerState { Filter = SamplerFilter.Nearest, Address = SamplerAddress.Repeat };
        smooth = Texture2D<float4>.FromTexels(new float4[] { new(1, 0, 0, 1), new(0, 1, 0, 1), new(0, 0, 1, 1), new(1, 1, 1, 1) }, 2, 2);
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

public enum Detail
{
    Low,
    Medium,
    High,
}

/// <summary>Runs once per combination of its keywords; fields are not readonly so the oracle can set them.</summary>
public static class Keywords
{
    [Keyword] public static bool Stripes;
    [Keyword(KeywordScope.Global)] public static Detail Quality;

    // Only the Stripes variants read the texture, only the High variants call Fine.
    [Texture(0)] public static Texture2D<float4> pattern;
    [Sampler(0)] public static SamplerState patternSampler;

    public static void Setup()
    {
        pattern = Texture2D<float4>.FromTexels(new float4[] { new(1, 1, 1, 1), new(0, 0, 0, 1), new(0, 0, 0, 1), new(1, 1, 1, 1) }, 2, 2);
        patternSampler = new SamplerState { Filter = SamplerFilter.Nearest, Address = SamplerAddress.Repeat };
    }

    static float Fine(float2 uv)
    {
        float sum = 0.0f;
        for (int i = 1; i <= 4; i++)
            sum += Frac(uv.x * i) * 0.25f;
        return sum;
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float3 color = new float3(uv, 0.25f);
        if (Stripes)
            color.xy *= pattern.Sample(patternSampler, uv * 2.0f).x * 0.5f + 0.5f;
        switch (Quality)
        {
            case Detail.Low:
                break;
            case Detail.Medium:
                color.z += 0.25f;
                break;
            default:
                color.z = Fine(uv);
                break;
        }
        if (Stripes && Quality == Detail.High)
            color.x = 1.0f - color.x;
        return new float4(color, Quality != Detail.Low || Stripes ? 1.0f : 0.5f);
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

/// <summary>Two bindless textures with different samplers and a bindless buffer, all reached through indices in a uniform block.</summary>
public static class BindlessResources
{
    public struct Params
    {
        public BindlessTexture checker;
        public BindlessTexture smooth;
        public BindlessBuffer colors;
        public uint count;
    }

    [Uniform(0)] public static Params data;

    public static void Setup()
    {
        float4[] pixels = new float4[16];
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                pixels[y * 4 + x] = new float4(x * 85 / 255.0f, y * 85 / 255.0f, (x ^ y) & 1, 1.0f);
        data.checker = Bindless.Register(Texture2D<float4>.FromTexels(pixels, 4, 4), new SamplerState { Filter = SamplerFilter.Nearest, Address = SamplerAddress.Repeat });
        data.smooth = Bindless.Register(Texture2D<float4>.FromTexels(new float4[] { new(1, 0, 0, 1), new(0, 1, 0, 1), new(0, 0, 1, 1), new(1, 1, 1, 1) }, 2, 2),
                                        new SamplerState { Filter = SamplerFilter.Linear, Address = SamplerAddress.Clamp });
        float4[] colors = { new(1.0f, 0.0f, 0.5f, 1.0f), new(0.0f, 1.0f, 0.25f, 1.0f), new(0.5f, 0.5f, 1.0f, 1.0f), new(0.25f, 0.75f, 0.0f, 1.0f) };
        uint[] words = new uint[16];
        for (int i = 0; i < 4; i++)
        {
            uint4 bits = AsUInt(colors[i]);
            words[i * 4] = bits.x;
            words[i * 4 + 1] = bits.y;
            words[i * 4 + 2] = bits.z;
            words[i * 4 + 3] = bits.w;
        }
        data.colors = Bindless.Register(words);
        data.count = 4u;
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float4 a = data.checker.Sample(uv * 2.0f);
        float4 b = data.smooth.SampleLevel(uv, 0.0f);
        uint cell = (uint)(uv.x * 4.0f);
        if (cell >= data.count)
            cell = data.count - 1u;
        float4 c = AsFloat(data.colors.Load4(cell * 16u));
        return new float4(a.x * 0.5f + c.x * 0.5f, b.y * 0.5f + c.y * 0.5f, a.z * 0.25f + b.z * 0.25f + c.z * 0.5f, data.colors.Size / 64.0f);
    }
}

/// <summary>The left half is drawn as a back face and the right half as a front face.</summary>
public static class FrontFaces
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv, [FrontFacing] bool front) =>
        front ? new float4(uv, 0.25f, 1.0f) : new float4(0.75f, uv.y, uv.x, 1.0f);
}

/// <summary>Writes a depth per pixel; where it is behind the probe (0.5) the probe color wins.</summary>
public static class Depths
{
    public struct Output
    {
        [Target(0)] public float4 color;
        [Depth] public float depth;
    }

    [FragmentShader]
    public static Output Fragment([Location(0)] float2 uv)
    {
        Output o;
        o.color = new float4(uv, 0.5f, 1.0f);
        // Never exactly 0.5, so rounding of the depth buffer cannot decide a pixel.
        o.depth = uv.x * 0.6f + uv.y * 0.3f + 0.0123f;
        return o;
    }
}

/// <summary>Writes vector components and matrix columns at positions computed per pixel.</summary>
public static class IndexedWrites
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        int i = (int)(uv.x * 4.0f);
        int j = (int)(uv.y * 3.0f);
        float4 v = new float4(0.1f, 0.2f, 0.3f, 0.4f);
        v[i] = 0.9f;
        v[3 - i] += 0.05f;
        float3x3 m = new float3x3(new float3(0.25f), new float3(0.5f), new float3(0.75f));
        m[j] = new float3(uv, 1.0f - uv.x);
        float3 column = m[2 - j];
        float3 sum = m[0] + m[1] + m[2];
        return new float4(v.x * 0.5f + column.x * 0.5f, v.y * 0.5f + sum.y * 0.2f, v.z * 0.5f + v.w * 0.5f, sum.z * 0.3f);
    }
}

/// <summary>Textures of other texel types: float ones sampled, integer ones loaded.</summary>
public static class TypedTextures
{
    [Texture(0)] public static Texture2D<float> heights;
    [Sampler(0)] public static SamplerState heightSampler;
    [Texture(1)] public static Texture2D<float2> offsets;
    [Sampler(1)] public static SamplerState offsetSampler;
    [Texture(2)] public static Texture2D<uint> ids;
    [Texture(3)] public static Texture2D<uint2> pairs;
    [Texture(4)] public static Texture2D<int> signs;
    [Texture(5)] public static Texture2D<int4> quads;

    public static void Setup()
    {
        heights = Texture2D<float>.FromTexels(new[] { 0.0f, 0.25f, 0.5f, 1.0f }, 2, 2);
        heightSampler = new SamplerState { Filter = SamplerFilter.Linear, Address = SamplerAddress.Clamp };
        offsets = Texture2D<float2>.FromTexels(new[] { new float2(0.125f, 0.75f), new float2(0.5f, 0.25f), new float2(1.0f, 0.0f), new float2(0.375f, 0.625f) }, 4, 1);
        offsetSampler = new SamplerState { Filter = SamplerFilter.Nearest, Address = SamplerAddress.Repeat };
        uint[] idTexels = new uint[16];
        for (int i = 0; i < 16; i++)
            idTexels[i] = (uint)(i * 37 + 3) & 255u;
        ids = Texture2D<uint>.FromTexels(idTexels, 4, 4);
        pairs = Texture2D<uint2>.FromTexels(new[] { new uint2(10, 200), new uint2(255, 0), new uint2(64, 128), new uint2(1, 2) }, 2, 2);
        signs = Texture2D<int>.FromTexels(new[] { -100, 50, 0, -255 }, 2, 2);
        quads = Texture2D<int4>.FromTexels(new[] { new int4(-1, 2, -3, 4), new int4(100, -100, 25, -25) }, 2, 1);
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float h = heights.Sample(heightSampler, uv);
        float2 o = offsets.SampleLevel(offsetSampler, uv, 0.0f);
        int2 cell = new int2((int)(uv.x * 4.0f), (int)(uv.y * 4.0f));
        uint id = ids.Load(cell, 0);
        uint2 pair = pairs.Load(new int2(cell.x / 2, cell.y / 2), 0);
        int sign = signs.Load(new int2(cell.y / 2, cell.x / 2), 0);
        int4 quad = quads.Load(new int2(cell.x / 2, 0), 0);
        int2 size = quads.Size;
        float a = (pair.x + pair.y) / 510.0f;
        float b = (sign + 255) / 510.0f;
        float c = (quad.x + quad.y + quad.z + quad.w + 200) / 400.0f;
        return new float4(h * 0.5f + o.x * 0.5f, id / 255.0f * 0.5f + a * 0.5f, b * 0.5f + c * 0.25f + o.y * 0.25f, size.x / 2.0f);
    }
}

/// <summary>A texture array, a volume and a cube; the cube is read at texel centers, so filtering across faces never matters.</summary>
public static class TextureKinds
{
    [Texture(0)] public static Texture2DArray<float4> layers;
    [Sampler(0)] public static SamplerState layerSampler;
    [Texture(1)] public static Texture3D<float4> volume;
    [Sampler(1)] public static SamplerState volumeSampler;
    [Texture(2)] public static TextureCube<float4> sky;
    [Sampler(2)] public static SamplerState skySampler;
    [Texture(3)] public static Texture2DArray<uint> codes;

    private static float4 Color(int i) => new float4((i * 37 % 256) / 255.0f, (i * 91 % 256) / 255.0f, (i * 53 % 256) / 255.0f, 1.0f);

    public static void Setup()
    {
        float4[] layerTexels = new float4[4 * 4 * 8];
        for (int i = 0; i < layerTexels.Length; i++)
            layerTexels[i] = Color(i);
        layers = Texture2DArray<float4>.FromTexels(layerTexels, 4, 4, 8);
        layerSampler = new SamplerState { Filter = SamplerFilter.Nearest, Address = SamplerAddress.Repeat };
        float4[] volumeTexels = new float4[4 * 4 * 4];
        for (int i = 0; i < volumeTexels.Length; i++)
            volumeTexels[i] = Color(i + 7);
        volume = Texture3D<float4>.FromTexels(volumeTexels, 4, 4, 4);
        volumeSampler = new SamplerState { Filter = SamplerFilter.Linear, Address = SamplerAddress.Clamp };
        float4[] faces = new float4[4 * 4 * 6];
        for (int i = 0; i < faces.Length; i++)
            faces[i] = Color(i + 100);
        sky = TextureCube<float4>.FromTexels(faces, 4);
        skySampler = new SamplerState { Filter = SamplerFilter.Nearest, Address = SamplerAddress.Clamp };
        uint[] codeTexels = new uint[2 * 2 * 2];
        for (int i = 0; i < codeTexels.Length; i++)
            codeTexels[i] = (uint)(i * 31 + 5);
        codes = Texture2DArray<uint>.FromTexels(codeTexels, 2, 2, 2);
    }

    // A direction that hits the center of texel (i, j) of a face, by the face rules of cube textures.
    private static float3 Direction(int face, int i, int j)
    {
        float sc = (i + 0.5f) / 2.0f - 1.0f, tc = (j + 0.5f) / 2.0f - 1.0f;
        switch (face)
        {
            case 0: return new float3(1.0f, -tc, -sc);
            case 1: return new float3(-1.0f, -tc, sc);
            case 2: return new float3(sc, 1.0f, tc);
            case 3: return new float3(sc, -1.0f, -tc);
            case 4: return new float3(sc, -tc, 1.0f);
            default: return new float3(-sc, -tc, -1.0f);
        }
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        int x = (int)(uv.x * 16.0f), y = (int)(uv.y * 16.0f);
        float4 a = layers.Sample(layerSampler, new float3(uv, (float)(y / 2)));
        float4 b = layers.Load(new int3(x / 4, y / 4, x % 8), 0);
        float4 c = volume.Sample(volumeSampler, new float3(uv.x, uv.y, 0.3f + uv.x * 0.4f));
        float4 d = sky.SampleLevel(skySampler, Direction((x + y) % 6, x % 4, y % 4), 0.0f);
        uint code = codes.Load(new int3(x % 2, y % 2, (x / 8) % 2), 0);
        int3 layerSize = layers.Size, volumeSize = volume.Size;
        int2 skySize = sky.Size;
        float sizes = (layerSize.x + layerSize.z + volumeSize.z + skySize.y) / 20.0f;
        return new float4(a.x * 0.5f + b.y * 0.5f, c.y * 0.5f + d.z * 0.5f, d.x * 0.5f + code / 255.0f * 0.5f, sizes);
    }
}

/// <summary>Shadow maps: comparison sampling of a 2D map (filtered), a layer of an array and a face of a cube.</summary>
public static class ShadowMaps
{
    [Texture(0)] public static Texture2D<float> sun;
    [Sampler(0)] public static SamplerComparisonState sunSampler;
    [Texture(1)] public static Texture2DArray<float> cascades;
    [Sampler(1)] public static SamplerComparisonState cascadeSampler;
    [Texture(2)] public static TextureCube<float> lamp;
    [Sampler(2)] public static SamplerComparisonState lampSampler;

    public static void Setup()
    {
        float[] sunDepths = new float[16];
        for (int i = 0; i < 16; i++)
            sunDepths[i] = (i * 7 % 16) / 17.0f + 0.013f;
        sun = Texture2D<float>.FromTexels(sunDepths, 4, 4);
        sunSampler = new SamplerComparisonState { Filter = SamplerFilter.Linear, Address = SamplerAddress.Clamp, Compare = CompareFunction.LessEqual };
        float[] cascadeDepths = new float[2 * 2 * 3];
        for (int i = 0; i < cascadeDepths.Length; i++)
            cascadeDepths[i] = (i * 5 % 12) / 13.0f + 0.021f;
        cascades = Texture2DArray<float>.FromTexels(cascadeDepths, 2, 2, 3);
        cascadeSampler = new SamplerComparisonState { Filter = SamplerFilter.Nearest, Address = SamplerAddress.Clamp, Compare = CompareFunction.Greater };
        float[] lampDepths = new float[2 * 2 * 6];
        for (int i = 0; i < lampDepths.Length; i++)
            lampDepths[i] = (i * 11 % 24) / 25.0f + 0.017f;
        lamp = TextureCube<float>.FromTexels(lampDepths, 2);
        lampSampler = new SamplerComparisonState { Filter = SamplerFilter.Nearest, Address = SamplerAddress.Clamp, Compare = CompareFunction.Less };
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        int x = (int)(uv.x * 16.0f), y = (int)(uv.y * 16.0f);
        float reference = (x * 13 + y * 7) % 32 / 33.0f + 0.004f;
        float a = sun.SampleCmp(sunSampler, uv, reference);
        float b = cascades.SampleCmpLevelZero(cascadeSampler, new float3((x % 2 + 0.5f) / 2.0f, (y % 2 + 0.5f) / 2.0f, (float)(x % 3)), reference);
        // The center of a texel of one face: the major axis is 1 and the others are at +-0.5.
        float s = x % 2 == 0 ? -0.5f : 0.5f, t = y % 2 == 0 ? -0.5f : 0.5f;
        float3 direction = (x + y) % 3 == 0 ? new float3(1.0f, s, t) : (x + y) % 3 == 1 ? new float3(s, -1.0f, t) : new float3(s, t, 1.0f);
        float c = lamp.SampleCmp(lampSampler, direction, reference);
        return new float4(a, b, c, 1.0f);
    }
}

/// <summary>Half precision locals, parameters and output.</summary>
public static class HalfMath
{
    private static half3 Tint(half3 c, half k) => c * k + new half3((half)0.125f);

    [FragmentShader]
    [return: Target(0)]
    public static half4 Fragment([Location(0)] float2 uv)
    {
        half2 h = (half2)uv;
        half3 c = Tint(new half3(h.x, h.y, (half)0.5f), (half)0.75f);
        half s = c.x * c.y + (half)0.25f;
        if (s > (half)0.5f)
            c.z = -c.z + (half)1.0f;
        return new half4(c.zyx, s);
    }
}

[System.Runtime.CompilerServices.InlineArray(4)]
public struct Weights
{
    private float element;
}

[System.Runtime.CompilerServices.InlineArray(3)]
public struct Colors
{
    private float4 element;
}

public struct Light
{
    public float4 color;
    public float4 direction;
}

[System.Runtime.CompilerServices.InlineArray(2)]
public struct Lights
{
    private Light element;
}

/// <summary>Fixed size arrays in locals, in a struct, as a parameter and in a uniform block, at constant and computed indices.</summary>
public static class InlineArrays
{
    public struct Scene
    {
        public Colors palette;
        public Lights lights;
        public float4 ambient;
    }

    public struct Mix
    {
        public Weights weights;
        public float bias;
    }

    [Uniform(0)] public static Scene scene;

    public static void Setup()
    {
        scene.palette[0] = new float4(1.0f, 0.0f, 0.0f, 1.0f);
        scene.palette[1] = new float4(0.0f, 1.0f, 0.0f, 1.0f);
        scene.palette[2] = new float4(0.0f, 0.0f, 1.0f, 1.0f);
        scene.lights[0] = new Light { color = new float4(0.5f, 0.25f, 0.0f, 1.0f), direction = new float4(1.0f, 0.0f, 0.0f, 0.0f) };
        scene.lights[1] = new Light { color = new float4(0.0f, 0.25f, 0.5f, 1.0f), direction = new float4(0.0f, 1.0f, 0.0f, 0.0f) };
        scene.ambient = new float4(0.125f);
    }

    private static float Total(Weights w, int count)
    {
        float sum = 0.0f;
        for (int i = 0; i < count; i++)
            sum += w[i];
        return sum;
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        int cell = (int)(uv.x * 3.0f);
        Mix mix;
        mix.bias = 0.0625f;
        mix.weights = new Weights();
        for (int i = 0; i < 4; i++)
            mix.weights[i] = (i + 1) * 0.0625f;
        mix.weights[cell] += uv.y * 0.25f;
        float4 color = scene.palette[cell] * Total(mix.weights, 3);
        Light light = scene.lights[(int)(uv.y * 2.0f)];
        color += light.color * (light.direction.x * uv.x + light.direction.y * uv.y);
        return new float4(color.xyz + scene.ambient.xyz + new float3(mix.bias), 1.0f);
    }
}

/// <summary>Local functions: static ones, ones that capture locals and parameters, and nested ones.</summary>
public static class LocalFunctions
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float gain = 0.5f + uv.x * 0.25f;
        int steps = 0;

        static float Square(float v) => v * v;

        float Scaled(float v)
        {
            steps++;
            return v * gain;
        }

        void Bump(ref float target)
        {
            float Twice(float v) => Scaled(v) * 2.0f;
            target += Twice(uv.y) * 0.25f;
        }

        float r = Scaled(Square(uv.x));
        float g = uv.y;
        Bump(ref g);
        gain = 0.75f;
        float b = Scaled(0.5f);
        return new float4(r, g * 0.5f, b, steps * 0.25f);
    }
}

/// <summary>Lambdas and delegates: cached ones without captures, ones that read and write captured variables, method
/// groups, a delegate type of the assembly, and delegates passed on through two helpers.</summary>
public static class Lambdas
{
    private delegate float Blend(float a, float b);

    private static float Apply(Func<float, float> f, float v) => f(v);

    private static float Twice(Func<float, float> f, float v) => Apply(f, Apply(f, v));

    private static float Square(float v) => v * v;

    private static float Mix(Blend blend, float a, float b) => blend(a, b);

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        Func<float, float> halve = v => v * 0.5f;
        float offset = uv.y * 0.25f;
        Func<float, float> shift = v => v + offset;
        int calls = 0;
        Action count = () => calls++;
        float r = Apply(halve, uv.x);
        float g = Twice(shift, uv.x * 0.5f);
        count();
        count();
        offset = 0.125f;
        float b = shift(Apply(Square, uv.y)) * 0.5f;
        float a = Mix((x, y) => x * 0.25f + y * 0.5f, uv.x, uv.y) + calls * 0.0625f;
        return new float4(r, g, b, a);
    }
}

/// <summary>Properties of user structs: auto properties with get, set and init, computed ones, an expression bodied
/// method, a readonly struct, an object initializer and computed static properties.</summary>
public static class Properties
{
    public struct Tint
    {
        private float strength;

        public float3 Color { get; set; }
        public float Bias { get; init; }

        public float Strength
        {
            readonly get => strength;
            set => strength = Saturate(value);
        }

        public readonly float3 Applied => Color * Strength + Bias;

        public readonly float Luma() => Dot(Applied, new float3(0.25f, 0.5f, 0.25f));
    }

    public readonly struct Range
    {
        public float Low { get; init; }
        public float High { get; init; }
        public float Width => High - Low;
    }

    public struct Surface
    {
        public float4 tint;

        public readonly float4 Tint => tint;
        public readonly float Alpha => tint.w * 0.5f;
    }

    [Uniform(0)] public static Surface surface;

    public static void Setup() => surface.tint = new float4(0.25f, 0.5f, 0.75f, 1.0f);

    private static float Half => 0.5f;

    private static float3 Ambient => new float3(0.0625f);

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        Tint t = new Tint { Color = new float3(uv, 0.25f), Bias = 0.125f * Half };
        t.Strength = uv.x * 2.0f;
        Range r = new Range { Low = 0.25f, High = 0.25f + uv.y * 0.5f };
        float3 c = t.Applied * 0.5f + Ambient;
        return new float4(c.x + surface.Tint.x * 0.25f, c.y, r.Width, t.Luma() * 0.5f + surface.Alpha * 0.25f);
    }
}

/// <summary>Methods of the shader library without an intrinsic, compiled from their C# bodies.</summary>
public static class LibraryMethods
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float4x4 m = Mul(float4x4.Translation(new float3(uv.x * 0.25f, 0.125f, 0.0f)), float4x4.Scale(new float3(0.5f, uv.y, 1.0f)));
        float4 p = Mul(m, new float4(uv, 0.5f, 1.0f));
        return new float4(p.xyz, 1.0f);
    }
}

/// <summary>Code of another assembly: its structs and enums, its functions, a readonly property, and a lambda of this
/// shader passed into it.</summary>
public static class Includes
{
    [Uniform(0)] public static LightData light;

    public static void Setup() =>
        light = new LightData { direction = new float4(0.0f, 0.6f, 0.8f, 0.5f), color = new float4(1.0f, 0.75f, 0.5f, 1.0f) };

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        SurfaceData s = new SurfaceData { albedo = new float3(uv, 0.5f), roughness = uv.x * 0.5f };
        float3 n = Normalize(new float3(uv.x - 0.5f, uv.y - 0.5f, 1.0f));
        float3 c = Lighting.Shade(s, n, light, uv.y > 0.5f ? Falloff.Squared : Falloff.Linear);
        float k = 0.5f + uv.y * 0.25f;
        float r = Lighting.Remap(uv.x, v => v * k);
        return new float4(c * 0.5f, r);
    }
}

/// <summary>Dynamic keywords are read from the keyword block at run time; the plain keyword still makes variants.</summary>
public static class DynamicKeywords
{
    [Keyword(Dynamic = true)] public static bool Glow;
    [Keyword(Dynamic = true)] public static Detail Level;
    [Keyword] public static bool Tint;

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float3 c = new float3(uv, 0.25f);
        if (Glow)
            c += 0.25f;
        if (Level == Detail.High)
            c.z = uv.x * uv.y;
        else if (Level == Detail.Medium)
            c.z = 0.5f;
        if (Tint)
            c.x *= 0.5f;
        return new float4(Saturate(c), 1.0f);
    }
}

/// <summary>Code the passes remove: a local nothing reads, a copy of a parameter, a parameter a helper never reads.</summary>
public static class DeadCode
{
    private static float Pick(float a, float b) => a * 0.5f;

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment([Location(0)] float2 uv)
    {
        float dead = uv.x * 3.0f;
        float2 copy = uv;
        return new float4(Pick(copy.y, uv.x * 2.0f), copy.x, 0.25f, 1.0f);
    }
}
