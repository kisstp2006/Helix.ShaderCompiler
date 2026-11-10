using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using HelixEngine.Shaders;

namespace Helix.ShaderOracle;

/// <summary>
/// Runs every case of Helix.ShaderTests.Cases on the CPU and writes what the GPU must reproduce: the image, the bytes of
/// the uniform structs and the textures with their samplers.
///
/// File layout (little endian): u32 size, u32 caseCount, u32 offset of the kernel section, then per case: u16 name length, name, size * size RGBA8 pixels
/// (rows top first), u32 uniformCount { u32 slot, u32 byteCount, bytes }, u32 textureCount { u32 slot, u32 width,
/// u32 height, u32 depth (layers, 6 for a cube), u32 kind (0 2D, 1 array, 2 volume, 3 cube), u32 filter, u32 address, u32 format,
/// u32 byteCount, texels }, u32 keywordCount { u16 name length, name, i32 value }.
/// Formats: 0 RGBA8 (float4 texels, rounded to 8 bits), 1 R32 float, 2 RG32 float, 3 R32 uint, 4 RG32 uint, 5 RGBA32 uint,
/// 6 R32 int, 7 RG32 int, 8 RGBA32 int.
/// Then per case: u32 bindlessTextureCount { the same as a texture without the slot }, u32
/// bindlessBufferCount { u32 byteCount, bytes }, in the order of their indices. Per kernel at the end: u32
/// bindlessBufferCount { u32 byteCount, bytes before, bytes after }.
/// A case with [Keyword] fields is written once per combination of their values.
///
/// Kernel section, the compute tests of Helix.ShaderTests.Kernels: u32 kernelCount, then per kernel: u16 name length, name,
/// 3 x u32 work group counts, u32 uniformCount { u32 slot, u32 byteCount, bytes }, u32 bufferCount { u32 slot, u32 writable,
/// u32 stride, u32 byteCount, bytes before, bytes after (writable buffers only) }, u32 storageTextureCount { u32 slot,
/// u32 width, u32 height, RGBA8 pixels after }.
/// </summary>
internal static class Program
{
    private const int Size = 16;

    private static byte[] Bytes<T>(T value) where T : unmanaged => MemoryMarshal.AsBytes(new ReadOnlySpan<T>(in value)).ToArray();

    private static byte Quantize(float v) => (byte)MathF.Round(Math.Clamp(v, 0.0f, 1.0f) * 255.0f);

    private static void WritePixel(BinaryWriter w, float4 c)
    {
        w.Write(Quantize(c.x));
        w.Write(Quantize(c.y));
        w.Write(Quantize(c.z));
        w.Write(Quantize(c.w));
    }

    // Every combination of the values the keyword fields of a case can take; one empty combination without keywords.
    private static List<(FieldInfo field, int value)[]> Variants(Type type)
    {
        List<(FieldInfo field, int value)[]> variants = new() { Array.Empty<(FieldInfo, int)>() };
        foreach (FieldInfo f in type.GetFields(BindingFlags.Public | BindingFlags.Static).Where(f => f.GetCustomAttribute<KeywordAttribute>() != null))
        {
            int[] values = f.FieldType.IsEnum ? Enum.GetValues(f.FieldType).Cast<object>().Select(Convert.ToInt32).ToArray() : new[] { 0, 1 };
            variants = variants.SelectMany(v => values.Select(value => v.Append((f, value)).ToArray())).ToList();
        }
        return variants;
    }

    private static byte[] ArrayBytes<T>(T[] items) where T : unmanaged => MemoryMarshal.AsBytes(items.AsSpan()).ToArray();

    // Bytes and element size of a StructuredBuffer or RWStructuredBuffer value.
    private static (byte[] bytes, int stride) BufferBytes(object buffer)
    {
        Type element = buffer.GetType().GetGenericArguments()[0];
        Array items = (Array)buffer.GetType().GetField("items", BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(buffer)!;
        MethodInfo bytesOf = typeof(Program).GetMethod(nameof(ArrayBytes), BindingFlags.NonPublic | BindingFlags.Static)!.MakeGenericMethod(element);
        return ((byte[])bytesOf.Invoke(null, new object[] { items })!, Marshal.SizeOf(element));
    }

    private static void WriteUniforms(BinaryWriter w, FieldInfo[] fields)
    {
        FieldInfo[] uniforms = fields.Where(f => f.GetCustomAttribute<UniformAttribute>() != null).ToArray();
        w.Write((uint)uniforms.Length);
        foreach (FieldInfo f in uniforms)
        {
            MethodInfo bytesOf = typeof(Program).GetMethod(nameof(Bytes), BindingFlags.NonPublic | BindingFlags.Static)!.MakeGenericMethod(f.FieldType);
            byte[] bytes = (byte[])bytesOf.Invoke(null, new[] { f.GetValue(null) })!;
            w.Write((uint)f.GetCustomAttribute<UniformAttribute>()!.Slot);
            w.Write((uint)bytes.Length);
            w.Write(bytes);
        }
    }

    // The color a fragment result leaves in the target. With a depth output the test draws a probe at depth 0.5 with the
    // test Less after the case, so the probe color wins where the case wrote a greater depth.
    private static float4 Color(object result)
    {
        if (result is float4 color)
            return color;
        FieldInfo[] fields = result.GetType().GetFields();
        float4 target = (float4)fields.First(f => f.GetCustomAttribute<TargetAttribute>() != null).GetValue(result)!;
        FieldInfo? depth = fields.FirstOrDefault(f => f.GetCustomAttribute<DepthAttribute>() != null);
        if (depth == null)
            return target;
        return System.Math.Clamp((float)depth.GetValue(result)!, 0.0f, 1.0f) <= 0.5f ? target : new float4(1.0f, 0.0f, 1.0f, 1.0f);
    }

    private static readonly Type[] TexelTypes =
        { typeof(float4), typeof(float), typeof(float2), typeof(uint), typeof(uint2), typeof(uint4), typeof(int), typeof(int2), typeof(int4) };

    // Any Texture2D<T>: its size, the sampler state, the format code and the texels as the GPU format holds them.
    private static void WriteTexture(BinaryWriter w, object texture, SamplerState sampler)
    {
        Type type = texture.GetType();
        object size = type.GetProperty("Size")!.GetValue(texture)!;
        Array texels = (Array)type.GetProperty("Texels")!.GetValue(texture)!;
        Type element = type.GetGenericArguments()[0];
        int format = Array.IndexOf(TexelTypes, element);
        if (format < 0)
            throw new InvalidOperationException($"no test format for {element.Name} texels");
        string[] kinds = { "Texture2D`1", "Texture2DArray`1", "Texture3D`1", "TextureCube`1" };
        int kind = Array.IndexOf(kinds, type.Name);
        (int x, int y, int z) dims = size is int3 s3 ? (s3.x, s3.y, s3.z) : size is int2 s2 ? (s2.x, s2.y, kind == 3 ? 6 : 1) : (0, 0, 0);
        w.Write((uint)dims.x);
        w.Write((uint)dims.y);
        w.Write((uint)dims.z);
        w.Write((uint)kind);
        w.Write((uint)sampler.Filter);
        w.Write((uint)sampler.Address);
        w.Write((uint)format);
        if (format == 0)
        {
            w.Write((uint)texels.Length * 4u);
            foreach (float4 pixel in (float4[])texels)
                WritePixel(w, pixel);
            return;
        }
        MethodInfo bytesOf = typeof(Program).GetMethod(nameof(ArrayBytes), BindingFlags.NonPublic | BindingFlags.Static)!.MakeGenericMethod(element);
        byte[] bytes = (byte[])bytesOf.Invoke(null, new object[] { texels })!;
        w.Write((uint)bytes.Length);
        w.Write(bytes);
    }

    private static void WriteBindless(BinaryWriter w)
    {
        w.Write((uint)Bindless.TextureCount);
        for (uint i = 0; i < Bindless.TextureCount; i++)
            WriteTexture(w, Bindless.Texture(i).Texture, Bindless.Texture(i).Sampler);
        w.Write((uint)Bindless.BufferCount);
        for (uint i = 0; i < Bindless.BufferCount; i++)
        {
            byte[] bytes = ArrayBytes(Bindless.Buffer(i));
            w.Write((uint)bytes.Length);
            w.Write(bytes);
        }
    }

    private static int WriteKernels(BinaryWriter w, Assembly shaders)
    {
        List<(Type type, MethodInfo run)> kernels = shaders.GetTypes()
            .Where(t => t.Namespace == "Helix.ShaderTests.Kernels")
            .Select(t => (t, t.GetMethod("Run", BindingFlags.Public | BindingFlags.Static)))
            .Where(k => k.Item2 != null && k.Item2.GetCustomAttribute<ComputeShaderAttribute>() != null)
            .OrderBy(k => k.t.Name, StringComparer.Ordinal)
            .Select(k => (k.t, k.Item2!))
            .ToList();
        w.Write((uint)kernels.Count);
        foreach ((Type type, MethodInfo run) in kernels)
        {
            Bindless.Clear();
            type.GetMethod("Setup", BindingFlags.Public | BindingFlags.Static)?.Invoke(null, null);
            List<byte[]> bindlessBefore = Enumerable.Range(0, Bindless.BufferCount).Select(i => ArrayBytes(Bindless.Buffer((uint)i))).ToList();
            byte[] name = System.Text.Encoding.ASCII.GetBytes(type.Name);
            w.Write((ushort)name.Length);
            w.Write(name);
            int3 groups = (int3)type.GetField("Groups", BindingFlags.Public | BindingFlags.Static)!.GetValue(null)!;
            w.Write((uint)groups.x);
            w.Write((uint)groups.y);
            w.Write((uint)groups.z);
            FieldInfo[] fields = type.GetFields(BindingFlags.Public | BindingFlags.Static);
            WriteUniforms(w, fields);

            FieldInfo[] buffers = fields.Where(f => f.GetCustomAttribute<StorageAttribute>() != null && f.FieldType.IsGenericType).ToArray();
            FieldInfo[] images = fields.Where(f => f.FieldType == typeof(RWTexture2D)).ToArray();
            List<byte[]> before = buffers.Select(f => BufferBytes(f.GetValue(null)!).bytes).ToList();
            ComputeShaderAttribute size = run.GetCustomAttribute<ComputeShaderAttribute>()!;
            object[] arguments = new object[1];
            for (uint z = 0; z < groups.z * size.Z; z++)
                for (uint y = 0; y < groups.y * size.Y; y++)
                    for (uint x = 0; x < groups.x * size.X; x++)
                    {
                        arguments[0] = new uint3(x, y, z);
                        run.Invoke(null, arguments);
                    }
            w.Write((uint)buffers.Length);
            for (int i = 0; i < buffers.Length; i++)
            {
                (byte[] after, int stride) = BufferBytes(buffers[i].GetValue(null)!);
                bool writable = buffers[i].FieldType.Name.StartsWith("RW", StringComparison.Ordinal);
                w.Write((uint)buffers[i].GetCustomAttribute<StorageAttribute>()!.Slot);
                w.Write(writable ? 1u : 0u);
                w.Write((uint)stride);
                w.Write((uint)before[i].Length);
                w.Write(before[i]);
                if (writable)
                    w.Write(after);
            }
            w.Write((uint)images.Length);
            foreach (FieldInfo f in images)
            {
                RWTexture2D image = (RWTexture2D)f.GetValue(null)!;
                w.Write((uint)f.GetCustomAttribute<StorageAttribute>()!.Slot);
                w.Write((uint)image.Size.x);
                w.Write((uint)image.Size.y);
                foreach (float4 pixel in image.Pixels!)
                    WritePixel(w, pixel);
            }
            w.Write((uint)bindlessBefore.Count);
            for (int i = 0; i < bindlessBefore.Count; i++)
            {
                w.Write((uint)bindlessBefore[i].Length);
                w.Write(bindlessBefore[i]);
                w.Write(ArrayBytes(Bindless.Buffer((uint)i)));
            }
        }
        return kernels.Count;
    }

    private static int Main(string[] args)
    {
        if (args.Length != 1)
        {
            Console.Error.WriteLine("usage: Helix.ShaderOracle <output file>");
            return 2;
        }
        Assembly shaders = typeof(Helix.ShaderTests.Cases.Fullscreen).Assembly;
        List<(Type type, MethodInfo fragment)> cases = shaders.GetTypes()
            .Where(t => t.Namespace == "Helix.ShaderTests.Cases")
            .Select(t => (t, t.GetMethod("Fragment", BindingFlags.Public | BindingFlags.Static)))
            .Where(c => c.Item2 != null && c.Item2.GetCustomAttribute<FragmentShaderAttribute>() != null)
            .OrderBy(c => c.t.Name, StringComparer.Ordinal)
            .Select(c => (c.t, c.Item2!))
            .ToList();

        using BinaryWriter w = new(File.Create(args[0]));
        w.Write((uint)Size);
        List<(Type type, MethodInfo fragment, (FieldInfo field, int value)[] keywords)> runs =
            cases.SelectMany(c => Variants(c.type).Select(v => (c.type, c.fragment, v))).ToList();
        w.Write((uint)runs.Count);
        w.Write(0u);
        foreach ((Type type, MethodInfo fragment, (FieldInfo field, int value)[] keywords) in runs)
        {
            Bindless.Clear();
            type.GetMethod("Setup", BindingFlags.Public | BindingFlags.Static)?.Invoke(null, null);
            foreach ((FieldInfo field, int value) in keywords)
                field.SetValue(null, field.FieldType.IsEnum ? Enum.ToObject(field.FieldType, value) : value != 0);
            byte[] name = System.Text.Encoding.ASCII.GetBytes(type.Name);
            w.Write((ushort)name.Length);
            w.Write(name);
            ParameterInfo[] parameters = fragment.GetParameters();
            object[] arguments = new object[parameters.Length];
            for (int y = 0; y < Size; y++)
            {
                for (int x = 0; x < Size; x++)
                {
                    // The test draws the left half as a back face and the right half as a front face.
                    for (int i = 0; i < parameters.Length; i++)
                        arguments[i] = parameters[i].GetCustomAttribute<FrontFacingAttribute>() != null
                                           ? x >= Size / 2
                                           : new float2((x + 0.5f) / Size, (y + 0.5f) / Size);
                    ShaderMath.Discarded = false;
                    float4 color = Color(fragment.Invoke(null, arguments)!);
                    // A discarded fragment leaves the cleared target, which is transparent black.
                    WritePixel(w, ShaderMath.Discarded ? default : color);
                }
            }

            FieldInfo[] fields = type.GetFields(BindingFlags.Public | BindingFlags.Static);
            WriteUniforms(w, fields);

            FieldInfo[] textures = fields.Where(f => f.GetCustomAttribute<TextureAttribute>() != null).ToArray();
            w.Write((uint)textures.Length);
            foreach (FieldInfo f in textures)
            {
                int slot = f.GetCustomAttribute<TextureAttribute>()!.Slot;
                object texture = f.GetValue(null)!;
                FieldInfo? samplerField = fields.FirstOrDefault(s => s.GetCustomAttribute<SamplerAttribute>()?.Slot == slot);
                SamplerState sampler = samplerField != null ? (SamplerState)samplerField.GetValue(null)! : default;
                w.Write((uint)slot);
                WriteTexture(w, texture, sampler);
            }

            w.Write((uint)keywords.Length);
            foreach ((FieldInfo field, int value) in keywords)
            {
                byte[] keyword = System.Text.Encoding.ASCII.GetBytes(field.Name);
                w.Write((ushort)keyword.Length);
                w.Write(keyword);
                w.Write(value);
            }
            WriteBindless(w);
        }
        uint kernelSection = (uint)w.BaseStream.Position;
        int kernels = WriteKernels(w, shaders);
        w.Seek(8, SeekOrigin.Begin);
        w.Write(kernelSection);
        Console.WriteLine($"{runs.Count} runs of {cases.Count} cases and {kernels} kernels written to {args[0]}");
        return 0;
    }
}
