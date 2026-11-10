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
/// File layout (little endian): u32 size, u32 caseCount, then per case: u16 name length, name, size * size RGBA8 pixels
/// (rows top first), u32 uniformCount { u32 slot, u32 byteCount, bytes }, u32 textureCount { u32 slot, u32 width,
/// u32 height, u32 filter, u32 address, RGBA8 pixels }, u32 keywordCount { u16 name length, name, i32 value }.
/// A case with [Keyword] fields is written once per combination of their values.
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
        foreach ((Type type, MethodInfo fragment, (FieldInfo field, int value)[] keywords) in runs)
        {
            type.GetMethod("Setup", BindingFlags.Public | BindingFlags.Static)?.Invoke(null, null);
            foreach ((FieldInfo field, int value) in keywords)
                field.SetValue(null, field.FieldType.IsEnum ? Enum.ToObject(field.FieldType, value) : value != 0);
            byte[] name = System.Text.Encoding.ASCII.GetBytes(type.Name);
            w.Write((ushort)name.Length);
            w.Write(name);
            object[] arguments = new object[1];
            for (int y = 0; y < Size; y++)
            {
                for (int x = 0; x < Size; x++)
                {
                    arguments[0] = new float2((x + 0.5f) / Size, (y + 0.5f) / Size);
                    ShaderMath.Discarded = false;
                    float4 color = (float4)fragment.Invoke(null, arguments)!;
                    // A discarded fragment leaves the cleared target, which is transparent black.
                    WritePixel(w, ShaderMath.Discarded ? default : color);
                }
            }

            FieldInfo[] fields = type.GetFields(BindingFlags.Public | BindingFlags.Static);
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

            FieldInfo[] textures = fields.Where(f => f.GetCustomAttribute<TextureAttribute>() != null).ToArray();
            w.Write((uint)textures.Length);
            foreach (FieldInfo f in textures)
            {
                int slot = f.GetCustomAttribute<TextureAttribute>()!.Slot;
                Texture2D texture = (Texture2D)f.GetValue(null)!;
                FieldInfo? samplerField = fields.FirstOrDefault(s => s.GetCustomAttribute<SamplerAttribute>()?.Slot == slot);
                SamplerState sampler = samplerField != null ? (SamplerState)samplerField.GetValue(null)! : default;
                w.Write((uint)slot);
                w.Write((uint)texture.Size.x);
                w.Write((uint)texture.Size.y);
                w.Write((uint)sampler.Filter);
                w.Write((uint)sampler.Address);
                foreach (float4 pixel in texture.Pixels!)
                    WritePixel(w, pixel);
            }

            w.Write((uint)keywords.Length);
            foreach ((FieldInfo field, int value) in keywords)
            {
                byte[] keyword = System.Text.Encoding.ASCII.GetBytes(field.Name);
                w.Write((ushort)keyword.Length);
                w.Write(keyword);
                w.Write(value);
            }
        }
        Console.WriteLine($"{runs.Count} runs of {cases.Count} cases written to {args[0]}");
        return 0;
    }
}
