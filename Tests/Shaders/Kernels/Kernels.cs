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

    [Storage(0, Format = StorageFormat.RGBA8)] public static RWTexture2D image;

    public static void Setup() => image = RWTexture2D.Create(16, 16);

    [ComputeShader(8, 8)]
    public static void Run([DispatchThreadId] uint3 id)
    {
        float2 uv = new float2(id.x, id.y) / 15.0f;
        image[new int2((int)id.x, (int)id.y)] = new float4(uv, ((id.x ^ id.y) & 1u) != 0u ? 1.0f : 0.25f, 1.0f);
    }
}
