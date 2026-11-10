using HelixEngine.Shaders;
using static HelixEngine.Shaders.ShaderMath;

// Every class here with a Run method is one compute test: the oracle runs it on the CPU for every thread of the dispatch
// (Groups work groups of the size in [ComputeShader]) and writes the buffers before and after; the GPU must leave the
// same bytes. Each thread only writes elements of its own, and the float math is exact, so the results do not depend on
// the order or the precision of the GPU.
namespace Helix.ShaderTests.Kernels;

public struct Count
{
    public uint value;
    public uint pad0, pad1, pad2;
}

public static class Squares
{
    public static readonly int3 Groups = new(4, 1, 1);

    [Uniform(0)] public static Count offset;
    [Storage(0)] public static RWStructuredBuffer<uint> values;

    public static void Setup()
    {
        offset.value = 7u;
        values = RWStructuredBuffer<uint>.FromArray(new uint[256]);
    }

    [ComputeShader(64)]
    public static void Run([DispatchThreadId] uint3 id) => values[(int)id.x] = id.x * id.x + offset.value;
}

public struct Particle
{
    public float4 position;
    public float2 velocity;
    public uint flags;
    public int id;
}

/// <summary>Reads structs from one buffer and writes changed structs to another, on a two dimensional grid.</summary>
public static class Particles
{
    public static readonly int3 Groups = new(2, 2, 1);

    [Storage(1)] public static StructuredBuffer<Particle> source;
    [Storage(2)] public static RWStructuredBuffer<Particle> moved;

    public static void Setup()
    {
        Particle[] items = new Particle[64];
        for (int i = 0; i < items.Length; i++)
        {
            items[i].position = new float4(i, i * 2, 1.0f, 0.5f);
            items[i].velocity = new float2(0.25f * i, -1.0f);
            items[i].flags = (uint)(i * 37) & 15u;
            items[i].id = 1000 - i;
        }
        source = StructuredBuffer<Particle>.FromArray(items);
        moved = RWStructuredBuffer<Particle>.FromArray(new Particle[64]);
    }

    [ComputeShader(4, 4)]
    public static void Run([DispatchThreadId] uint3 id)
    {
        int index = (int)(id.y * 8u + id.x);
        Particle p = source[index];
        p.position.xy += p.velocity * 2.0f;
        p.position.w = id.y;
        if ((p.flags & 1u) != 0u)
            p.velocity = -p.velocity;
        p.flags |= id.x << 8;
        p.id += source.Length;
        moved[index] = p;
    }
}

/// <summary>Updates a buffer in place with a helper that loops, in three dimensions.</summary>
public static class InPlace
{
    public static readonly int3 Groups = new(1, 2, 2);

    [Storage(0)] public static RWStructuredBuffer<float4> cells;
    [Storage(3)] public static StructuredBuffer<int> weights;

    public static void Setup()
    {
        float4[] items = new float4[32];
        for (int i = 0; i < items.Length; i++)
            items[i] = new float4(i, 1.0f, 0.5f * i, 2.0f);
        cells = RWStructuredBuffer<float4>.FromArray(items);
        weights = StructuredBuffer<int>.FromArray(new[] { 1, 2, 4, 8 });
    }

    static int Total(int count)
    {
        int sum = 0;
        for (int i = 0; i < count; i++)
        {
            if (i >= weights.Length)
                return sum;
            sum += weights[i];
        }
        return sum;
    }

    [ComputeShader(8, 1, 1)]
    public static void Run([DispatchThreadId] uint3 id)
    {
        int index = (int)(id.x + id.y * 8u + id.z * 16u);
        float4 cell = cells[index];
        cell.x = cell.x * 2.0f + Total((int)id.x);
        cell.zw = cell.wz;
        cells[index] = cell;
    }
}

/// <summary>Counters shared by all threads: the sums do not depend on the order the threads run in.</summary>
public static class Histogram
{
    public static readonly int3 Groups = new(4, 1, 1);

    [Storage(0)] public static StructuredBuffer<uint> samples;
    [Storage(1)] public static RWStructuredBuffer<uint> bins;
    [Storage(2)] public static RWStructuredBuffer<int> balance;

    public static void Setup()
    {
        uint[] items = new uint[64];
        for (int i = 0; i < items.Length; i++)
            items[i] = (uint)(i * i + 3 * i);
        samples = StructuredBuffer<uint>.FromArray(items);
        bins = RWStructuredBuffer<uint>.FromArray(new uint[8]);
        balance = RWStructuredBuffer<int>.FromArray(new[] { 100, 0 });
    }

    [ComputeShader(16)]
    public static void Run([DispatchThreadId] uint3 id)
    {
        uint sample = samples[(int)id.x];
        InterlockedAdd(bins, (int)(sample & 3u), 1u);
        // Every thread sees a different count before its own add; their sum is the same in any order.
        uint before = InterlockedAdd(bins, 4, 1u);
        InterlockedAdd(bins, 5, before);
        if ((sample & 1u) == 0u)
            InterlockedAdd(balance, 0, -(int)id.x);
        else
            InterlockedAdd(balance, 1, (int)sample);
    }
}

/// <summary>Writes every texel of a storage texture.</summary>
public static class Paint
{
    public static readonly int3 Groups = new(2, 2, 1);

    [Storage(0, Format = StorageFormat.RGBA8)] public static RWTexture2D<float4> image;

    public static void Setup() => image = RWTexture2D<float4>.Create(16, 16);

    [ComputeShader(8, 8)]
    public static void Run([DispatchThreadId] uint3 id)
    {
        float2 uv = new float2(id.x, id.y) / 15.0f;
        image[new int2((int)id.x, (int)id.y)] = new float4(uv, ((id.x ^ id.y) & 1u) != 0u ? 1.0f : 0.25f, 1.0f);
    }
}

/// <summary>Reads one bindless buffer and writes another, both named by indices in a uniform block.</summary>
public static class BindlessWrites
{
    public static readonly int3 Groups = new(2, 1, 1);

    public struct Params
    {
        public BindlessBuffer source;
        public BindlessBuffer target;
        public uint scale;
        public uint pad;
    }

    [Uniform(0)] public static Params data;

    public static void Setup()
    {
        uint[] words = new uint[64];
        for (int i = 0; i < words.Length; i++)
            words[i] = (uint)(i * 3 + 1);
        data.source = Bindless.Register(words);
        data.target = Bindless.Register(new uint[130]);
        data.scale = 5u;
    }

    [ComputeShader(32)]
    public static void Run([DispatchThreadId] uint3 id)
    {
        uint offset = id.x * 4u;
        if (offset >= data.source.Size)
            return;
        uint value = data.source.Load(offset);
        data.target.Store(offset, value * data.scale + id.x);
        // Even threads also write two words past the first 64, so no two threads write the same word.
        if ((id.x & 1u) == 0u)
            data.target.Store2(256u + offset, data.source.Load2(offset) + new uint2(1000u, data.target.Size));
    }
}

/// <summary>Storage textures read and written in place, in the formats a shader can read, and one only written.</summary>
public static class ImageReadWrite
{
    public static readonly int3 Groups = new(2, 2, 1);

    [Storage(0)] public static RWTexture2D<float> heat;
    [Storage(1)] public static RWTexture2D<uint> counts;
    [Storage(2)] public static RWTexture2D<int> signs;
    [Storage(3, Format = StorageFormat.RGBA16F)] public static RWTexture2D<float4> colors;

    public static void Setup()
    {
        float[] heats = new float[256];
        uint[] values = new uint[256];
        for (int i = 0; i < 256; i++)
        {
            heats[i] = i * 0.25f;
            values[i] = (uint)(i * 3);
        }
        heat = RWTexture2D<float>.FromTexels(heats, 16, 16);
        counts = RWTexture2D<uint>.FromTexels(values, 16, 16);
        signs = RWTexture2D<int>.Create(16, 16);
        colors = RWTexture2D<float4>.Create(16, 16);
    }

    [ComputeShader(8, 8)]
    public static void Run([DispatchThreadId] uint3 id)
    {
        int2 p = new int2((int)id.x, (int)id.y);
        int2 size = heat.Size;
        heat[p] = heat[p] * 2.0f + size.x;
        uint count = counts[p] + id.x;
        counts[p] = count;
        signs[p] = -(int)id.y * 3 + (int)count;
        colors[p] = new float4(id.x * 0.25f, id.y * 0.5f, 0.125f, 1.0f);
    }
}

public enum Curve
{
    Linear,
    Square,
    Cube,
}

/// <summary>Runs once per combination of its keywords; fields are not readonly so the oracle can set them.</summary>
public static class Shaped
{
    public static readonly int3 Groups = new(2, 1, 1);

    [Keyword] public static bool Negate;
    [Keyword] public static Curve Shape;

    [Storage(0)] public static RWStructuredBuffer<float> values;

    public static void Setup() => values = RWStructuredBuffer<float>.FromArray(new float[128]);

    [ComputeShader(64)]
    public static void Run([DispatchThreadId] uint3 id)
    {
        float x = id.x * 0.125f;
        float y = Shape == Curve.Square ? x * x : Shape == Curve.Cube ? x * x * x : x;
        values[(int)id.x] = Negate ? -y : y;
    }
}

/// <summary>Each thread sums the elements before its own: an inclusive scan with a loop of its own length.</summary>
public static class Scan
{
    public static readonly int3 Groups = new(1, 1, 1);

    [Storage(0)] public static StructuredBuffer<uint> input;
    [Storage(1)] public static RWStructuredBuffer<uint> sums;

    public static void Setup()
    {
        uint[] values = new uint[64];
        for (int i = 0; i < values.Length; i++)
            values[i] = (uint)(i * 7 % 13);
        input = StructuredBuffer<uint>.FromArray(values);
        sums = RWStructuredBuffer<uint>.FromArray(new uint[64]);
    }

    [ComputeShader(64)]
    public static void Run([DispatchThreadId] uint3 id)
    {
        uint total = 0u;
        for (int i = 0; i <= (int)id.x; i++)
            total += input[i];
        sums[(int)id.x] = total;
    }
}

/// <summary>A product of two 8 by 8 integer matrices, one element per thread.</summary>
public static class MatrixProduct
{
    public static readonly int3 Groups = new(1, 1, 1);

    [Storage(0)] public static StructuredBuffer<int> left;
    [Storage(1)] public static StructuredBuffer<int> right;
    [Storage(2)] public static RWStructuredBuffer<int> product;

    public static void Setup()
    {
        int[] a = new int[64], b = new int[64];
        for (int i = 0; i < 64; i++)
        {
            a[i] = i % 5 - 2;
            b[i] = i % 7 - 3;
        }
        left = StructuredBuffer<int>.FromArray(a);
        right = StructuredBuffer<int>.FromArray(b);
        product = RWStructuredBuffer<int>.FromArray(new int[64]);
    }

    [ComputeShader(8, 8)]
    public static void Run([DispatchThreadId] uint3 id)
    {
        int row = (int)id.y, column = (int)id.x, sum = 0;
        for (int k = 0; k < 8; k++)
            sum += left[row * 8 + k] * right[k * 8 + column];
        product[row * 8 + column] = sum;
    }
}

/// <summary>A storage texture written from integer math on a 2D grid.</summary>
public static class ImageGrid
{
    public static readonly int3 Groups = new(2, 2, 1);

    [Storage(0)] public static RWTexture2D<uint> cells;

    public static void Setup() => cells = RWTexture2D<uint>.Create(16, 16);

    [ComputeShader(8, 8)]
    public static void Run([DispatchThreadId] uint3 id)
    {
        uint x = id.x, y = id.y;
        cells[new int2((int)x, (int)y)] = (x * 31u + y * 17u) ^ (x << 4) ^ (y << 8);
    }
}

public struct Cell
{
    public float4 value;
    public Weights weights;
}

public struct Weights
{
    public float4 near;
    public float4 far;
}

/// <summary>Buffers of nested structs read and written field by field.</summary>
public static class NestedBuffers
{
    public static readonly int3 Groups = new(1, 1, 1);

    [Storage(0)] public static RWStructuredBuffer<Cell> cells;

    public static void Setup()
    {
        Cell[] items = new Cell[32];
        for (int i = 0; i < items.Length; i++)
        {
            items[i].value = new float4(i, 0.5f * i, 1.0f, 2.0f);
            items[i].weights.near = new float4(0.25f, 0.5f, 0.75f, 1.0f);
            items[i].weights.far = new float4(i % 4);
        }
        cells = RWStructuredBuffer<Cell>.FromArray(items);
    }

    [ComputeShader(32)]
    public static void Run([DispatchThreadId] uint3 id)
    {
        int i = (int)id.x;
        Cell c = cells[i];
        c.value = c.value * c.weights.near + c.weights.far;
        c.weights.far.x = c.value.y;
        cells[i] = c;
    }
}
