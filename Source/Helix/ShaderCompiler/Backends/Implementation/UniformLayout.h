#pragma once

#include <Helix/Core/Strings/Format.h>
#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    // GPU layouts: std140 for uniform blocks (which also satisfies the D3D constant buffer rules), std430 for buffer
    // elements, and the sequential layout of the C# structs.
    struct UniformLayout
    {
        u32 align, size;
    };

    inline UniformLayout Std140(const Module& m, Type t)
    {
        if (t.base == BaseType::Array)
        {
            // Every element starts on 16 bytes.
            const ArrayType& a = m.arrays[t.structIndex];
            UniformLayout e = Std140(m, a.element);
            u32 stride = u32(AlignUp(e.size, 16));
            return {Max(e.align, 16u), stride * a.length};
        }
        if (t.base == BaseType::Struct)
        {
            u32 offset = 0, align = 16;
            for (const StructField& f : m.structs[t.structIndex].fields)
            {
                UniformLayout l = Std140(m, f.type);
                offset = u32(AlignUp(offset, l.align)) + l.size;
                align = Max(align, l.align);
            }
            return {align, u32(AlignUp(offset, align))};
        }
        if (t.IsMatrix())
            return {16, 16u * t.cols};
        if (t.rows == 1)
            return {4, 4};
        return {t.rows == 2 ? 8u : 16u, 4u * t.rows};
    }

    // Size of the type as C# lays it out: fields in order, four bytes per component.
    inline u32 SequentialSize(const Module& m, Type t)
    {
        if (t.base == BaseType::Array)
            return m.arrays[t.structIndex].length * SequentialSize(m, m.arrays[t.structIndex].element);
        if (t.base != BaseType::Struct)
            return 4u * t.rows * t.cols;
        u32 size = 0;
        for (const StructField& f : m.structs[t.structIndex].fields)
            size += SequentialSize(m, f.type);
        return size;
    }

    // std430, the layout of storage buffer elements: like std140, but structs and arrays are not rounded up to 16 bytes.
    inline UniformLayout Std430(const Module& m, Type t)
    {
        if (t.base == BaseType::Array)
        {
            const ArrayType& a = m.arrays[t.structIndex];
            UniformLayout e = Std430(m, a.element);
            return {e.align, u32(AlignUp(e.size, e.align)) * a.length};
        }
        if (t.base == BaseType::Struct)
        {
            u32 offset = 0, align = 4;
            for (const StructField& f : m.structs[t.structIndex].fields)
            {
                UniformLayout l = Std430(m, f.type);
                offset = u32(AlignUp(offset, l.align)) + l.size;
                align = Max(align, l.align);
            }
            return {align, u32(AlignUp(offset, align))};
        }
        u32 align = t.rows == 1 ? 4u : t.rows == 2 ? 8u : 16u;
        return {align, t.IsMatrix() ? u32(AlignUp(4u * t.rows, align)) * t.cols : 4u * t.rows};
    }

    // Buffer elements are copied as the C# values lie in memory and Direct3D reads them packed, so every field must sit
    // at the same place in C#, in std430 and packed: no three component vectors or bools, no holes.
    inline bool CheckBufferLayout(const Module& m, Type t, StringView path, String& error)
    {
        if (t.base == BaseType::Array)
        {
            const ArrayType& a = m.arrays[t.structIndex];
            UniformLayout e = Std430(m, a.element);
            if (u32(AlignUp(e.size, e.align)) != SequentialSize(m, a.element))
            {
                error = Format("buffer array {} has elements of {} bytes in C# but {} on the GPU; pad the element", path, SequentialSize(m, a.element),
                               u32(AlignUp(e.size, e.align)));
                return false;
            }
            return CheckBufferLayout(m, a.element, Format("{}[0]", path), error);
        }
        if (t.base == BaseType::Struct)
        {
            u32 offset = 0;
            for (const StructField& f : m.structs[t.structIndex].fields)
            {
                String name = Format("{}.{}", path, f.name);
                if (!CheckBufferLayout(m, f.type, name, error))
                    return false;
                UniformLayout l = Std430(m, f.type);
                if (offset % l.align)
                {
                    error = Format("buffer element field {} is at byte {} in C# but needs a multiple of {} on the GPU; reorder the fields or add padding",
                                   name, offset, l.align);
                    return false;
                }
                offset += SequentialSize(m, f.type);
            }
            UniformLayout whole = Std430(m, t);
            if (whole.size != offset)
            {
                error = Format("buffer element {} is {} bytes in C# but {} on the GPU; add padding at its end", path, offset, whole.size);
                return false;
            }
            return true;
        }
        if (t.half)
        {
            error = Format("buffer element {} is half; buffers take float, since the GPU layout has no 16 bit floats", path);
            return false;
        }
        if (t.base == BaseType::Bool || t.rows == 3)
        {
            error = Format("buffer element {} is a {}; use {}", path, t.base == BaseType::Bool ? "bool" : "three component vector or 3x3 matrix",
                           t.base == BaseType::Bool ? "an int or uint" : "four components, which the GPU stores anyway");
            return false;
        }
        return true;
    }

    // Uniform blocks are laid out by std140 (every array element on 16 bytes); the C# struct does not have to match,
    // since materials and the renderer write members at the reflected offsets. Refused: what has no fixed GPU layout.
    inline bool CheckUniformLayout(const Module& m, Type t, StringView path, String& error, u32 depth = 0)
    {
        if (depth > 64)
        {
            error = Format("uniform {} nests too deep", path);
            return false;
        }
        for (const StructField& f : m.structs[t.structIndex].fields)
        {
            String name = Format("{}.{}", path, f.name);
            Type type = f.type;
            // Arrays: the checks of their elements.
            while (type.base == BaseType::Array)
            {
                type = m.arrays[type.structIndex].element;
                name = Format("{}[0]", name);
            }
            if (type.base == BaseType::Bool)
            {
                error = Format("uniform field {} is a bool; use an int or uint, bools have no fixed layout", name);
                return false;
            }
            if (type.half)
            {
                error = Format("uniform field {} is half; uniform blocks take float, since the GPU layout has no 16 bit floats", name);
                return false;
            }
            if (type.IsMatrix() && (type.cols != 4 || type.rows != 4))
            {
                error = Format("uniform field {} is a {}x{} matrix, which the GPU stores with padded columns; use float4x4", name, u32(type.rows),
                               u32(type.cols));
                return false;
            }
            if (type.base == BaseType::Struct && !CheckUniformLayout(m, type, name, error, depth + 1))
                return false;
        }
        return true;
    }

    // Byte offsets of the numeric leaves of a uniform value (scalars, vectors, matrix columns) in std140, in the order
    // of the type's fields and elements.
    inline void Std140Leaves(const Module& m, Type t, u32 base, Array<u32>& out)
    {
        if (t.base == BaseType::Array)
        {
            const ArrayType& a = m.arrays[t.structIndex];
            u32 stride = u32(AlignUp(Std140(m, a.element).size, 16));
            for (u32 i = 0; i < a.length; i++)
                Std140Leaves(m, a.element, base + i * stride, out);
        }
        else if (t.base == BaseType::Struct)
        {
            u32 offset = base;
            for (const StructField& f : m.structs[t.structIndex].fields)
            {
                UniformLayout l = Std140(m, f.type);
                offset = u32(AlignUp(offset, l.align));
                Std140Leaves(m, f.type, offset, out);
                offset += l.size;
            }
        }
        else if (t.IsMatrix())
        {
            for (u32 c = 0; c < t.cols; c++)
                out.Push(base + 16 * c);
        }
        else if (t.IsNumeric())
            out.Push(base);
    }
}
