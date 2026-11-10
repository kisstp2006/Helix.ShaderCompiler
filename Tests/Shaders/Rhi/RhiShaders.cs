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
    [Texture(3)] public static Texture2D image;
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
    [Texture(0)] public static Texture2D image;
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
