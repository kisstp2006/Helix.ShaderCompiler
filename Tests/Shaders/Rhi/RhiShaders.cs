using HelixEngine.Shaders;
using static HelixEngine.Shaders.ShaderMath;

// The shaders of the Helix.RHI tests. Helix.RHI sits below this compiler, so it cannot build them itself: the export tool
// writes them as a C++ header (GLSL, GLSL ES and Direct3D bytecode) that is copied into the Helix.RHI tests.
namespace Helix.ShaderTests.Rhi;

/// <summary>Position only; pairs with the fragment shaders that take no input.</summary>
public static class Plain
{
    [VertexShader]
    [return: Position]
    public static float4 Vertex([Location(0)] float2 p) => new float4(p, 0.5f, 1.0f);
}

public static class Green
{
    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment() => new float4(0.0f, 1.0f, 0.0f, 1.0f);
}

public struct ColorBlock
{
    public float4 color;
}

/// <summary>Color from a uniform block in slot 0.</summary>
public static class Solid
{
    [Uniform(0)] public static ColorBlock block;

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment() => block.color;
}

/// <summary>Color from a uniform block in slot 2.</summary>
public static class Tint
{
    [Uniform(2)] public static ColorBlock material;

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment() => material.color;
}

public struct ColorVaryings
{
    [Position] public float4 position;
    [Location(0)] public float4 color;
}

public static class Color
{
    [VertexShader]
    public static ColorVaryings Vertex([Location(0)] float2 p, [Location(1)] float4 c)
    {
        ColorVaryings o;
        o.position = new float4(p, 0.5f, 1.0f);
        o.color = c;
        return o;
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment(ColorVaryings input) => input.color;
}

/// <summary>Per instance offset and color.</summary>
public static class Instanced
{
    [VertexShader]
    public static ColorVaryings Vertex([Location(0)] float2 p, [Location(1)] float2 offset, [Location(2)] float4 c)
    {
        ColorVaryings o;
        o.position = new float4(p * 0.5f + offset, 0.5f, 1.0f);
        o.color = c;
        return o;
    }
}

public struct UvVaryings
{
    [Position] public float4 position;
    [Location(0)] public float2 uv;
}

public static class Textured
{
    [Texture(3)] public static Texture2D<float4> image;
    [Sampler(3)] public static SamplerState imageSampler;

    [VertexShader]
    public static UvVaryings Vertex([Location(0)] float2 p, [Location(1)] float2 uv)
    {
        UvVaryings o;
        o.position = new float4(p, 0.5f, 1.0f);
        o.uv = uv;
        return o;
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment(UvVaryings input) => image.Sample(imageSampler, input.uv);
}

/// <summary>Depth of the whole draw from the uniform block both stages share.</summary>
public static class Depth
{
    public struct Draw
    {
        public float4 color;
        public float depth;
    }

    [Uniform(0)] public static Draw draw;

    [VertexShader]
    [return: Position]
    public static float4 Vertex([Location(0)] float2 p) => new float4(p, draw.depth, 1.0f);

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment() => draw.color;
}

/// <summary>Small shapes placed by the uniform block.</summary>
public static class Placed
{
    public struct Draw
    {
        public float4 color;
        public float2 offset;
    }

    [Uniform(0)] public static Draw draw;

    [VertexShader]
    [return: Position]
    public static float4 Vertex([Location(0)] float2 p) => new float4(p * 0.01f + draw.offset, 0.5f, 1.0f);

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment() => draw.color;
}

/// <summary>Vertex color, optionally multiplied by a texture.</summary>
public static class Scene
{
    public struct Mode
    {
        public uint mode;
    }

    public struct Varyings
    {
        [Position] public float4 position;
        [Location(0)] public float4 color;
        [Location(1)] public float2 uv;
    }

    [Uniform(0)] public static Mode settings;
    [Texture(0)] public static Texture2D<float4> image;
    [Sampler(0)] public static SamplerState imageSampler;

    [VertexShader]
    public static Varyings Vertex([Location(0)] float3 p, [Location(1)] float4 c, [Location(2)] float2 uv)
    {
        Varyings o;
        o.position = new float4(p, 1.0f);
        o.color = c;
        o.uv = uv;
        return o;
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment(Varyings input)
    {
        if (settings.mode == 0u)
            return input.color;
        return image.Sample(imageSampler, input.uv) * input.color;
    }
}

/// <summary>Squares plus a uniform into a buffer.</summary>
public static class Fill
{
    public struct Params
    {
        public uint k;
    }

    [Uniform(0)] public static Params parameters;
    [Storage(0)] public static RWStructuredBuffer<uint> values;

    [ComputeShader(64)]
    public static void Compute([DispatchThreadId] uint3 id) => values[(int)id.x] = id.x * id.x + parameters.k;
}

/// <summary>A gradient into a storage texture.</summary>
public static class Gradient
{
    [Storage(0)] public static RWTexture2D<float4> image;

    [ComputeShader(8, 8)]
    public static void Compute([DispatchThreadId] uint3 id) =>
        image[new int2((int)id.x, (int)id.y)] = new float4(id.x / 15.0f, id.y / 15.0f, 0.0f, 1.0f);
}

/// <summary>Appends the two triangles of every visible quad and counts the vertices in the draw arguments.</summary>
public static class Cull
{
    [Storage(0)] public static RWStructuredBuffer<float2> vertices;
    [Storage(1)] public static StructuredBuffer<uint> visible;
    [Storage(2)] public static RWStructuredBuffer<uint> arguments;

    [ComputeShader(4)]
    public static void Compute([DispatchThreadId] uint3 id)
    {
        if (visible[(int)id.x] == 0u)
            return;
        int first = (int)InterlockedAdd(arguments, 0, 6u);
        float x0 = id.x * 0.5f - 1.0f, x1 = x0 + 0.5f;
        vertices[first] = new float2(x0, -1.0f);
        vertices[first + 1] = new float2(x1, -1.0f);
        vertices[first + 2] = new float2(x0, 1.0f);
        vertices[first + 3] = new float2(x1, -1.0f);
        vertices[first + 4] = new float2(x1, 1.0f);
        vertices[first + 5] = new float2(x0, 1.0f);
    }
}

/// <summary>A quad placed by x0 and x1 that samples the bindless texture named in the same block.</summary>
public static class BindlessQuad
{
    public struct Draw
    {
        public BindlessTexture texture;
        public float x0, x1, pad;
    }

    [Uniform(0)] public static Draw draw;

    [VertexShader]
    public static UvVaryings Vertex([Location(0)] float2 p)
    {
        UvVaryings o;
        o.position = new float4((draw.x0 + (draw.x1 - draw.x0) * p.x) * 2.0f - 1.0f, p.y * 2.0f - 1.0f, 0.5f, 1.0f);
        o.uv = p;
        return o;
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment(UvVaryings input) => draw.texture.Sample(input.uv);
}

/// <summary>A quad placed by x0 and x1 whose color is read from a bindless buffer, 16 bytes in.</summary>
public static class BindlessColor
{
    public struct Draw
    {
        public BindlessBuffer colors;
        public float x0, x1, pad;
    }

    [Uniform(0)] public static Draw draw;

    [VertexShader]
    [return: Position]
    public static float4 Vertex([Location(0)] float2 p) =>
        new float4((draw.x0 + (draw.x1 - draw.x0) * p.x) * 2.0f - 1.0f, p.y * 2.0f - 1.0f, 0.5f, 1.0f);

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment() => AsFloat(draw.colors.Load4(16u));
}

/// <summary>Writes id * 3 + its own index into every word of a bindless buffer.</summary>
public static class HeapWrites
{
    public struct Params
    {
        public BindlessBuffer buffer;
        public uint pad0, pad1, pad2;
    }

    [Uniform(0)] public static Params parameters;

    [ComputeShader(64)]
    public static void Compute([DispatchThreadId] uint3 id)
    {
        if (id.x * 4u >= parameters.buffer.Size)
            return;
        parameters.buffer.Store(id.x * 4u, id.x * 3u + parameters.buffer.Index);
    }
}

/// <summary>The rotating triangle of the RHI example: rotation (cos, sin) and aspect in the frame block.</summary>
public static class Spinning
{
    public struct Frame
    {
        public float4 rotation;
    }

    public struct Varyings
    {
        [Position] public float4 position;
        [Location(0)] public float3 color;
    }

    [Uniform(0)] public static Frame frame;

    [VertexShader]
    public static Varyings Vertex([Location(0)] float2 p, [Location(1)] float3 c)
    {
        float4 r = frame.rotation;
        Varyings o;
        o.position = new float4((p.x * r.x - p.y * r.y) * r.z, p.x * r.y + p.y * r.x, 0.5f, 1.0f);
        o.color = c;
        return o;
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment(Varyings input) => new float4(input.color, 1.0f);
}

/// <summary>Reads a shadow map with a comparison sampler over the whole target: red where lit, green where shadowed.</summary>
public static class ShadowReceiver
{
    public struct Receiver
    {
        public float reference;
        public float pad0, pad1, pad2;
    }

    [Uniform(0)] public static Receiver receiver;
    [Texture(0)] public static Texture2D<float> shadow;
    [Sampler(0)] public static SamplerComparisonState shadowSampler;

    [VertexShader]
    public static UvVaryings Vertex([Location(0)] float2 p)
    {
        UvVaryings o;
        o.position = new float4(p, 0.5f, 1.0f);
        o.uv = new float2(p.x * 0.5f + 0.5f, 0.5f - p.y * 0.5f);
        return o;
    }

    [FragmentShader]
    [return: Target(0)]
    public static float4 Fragment(UvVaryings input)
    {
        float lit = shadow.SampleCmpLevelZero(shadowSampler, input.uv, receiver.reference);
        return new float4(lit, 1.0f - lit, 0.0f, 1.0f);
    }
}
