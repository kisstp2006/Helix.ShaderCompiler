#pragma once

#include <Helix/Core/Strings/Format.h>
#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    // Uniform structs are copied to the GPU as the C# struct lies in memory. That works when every field sits where the
    // GPU layout rules (std140, which also satisfies the D3D constant buffer rules) put it; these helpers check it.
    struct UniformLayout
    {
        u32 align, size;
    };

    inline UniformLayout Std140(const Module& m, Type t)
    {
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
        if (t.base != BaseType::Struct)
            return 4u * t.rows * t.cols;
        u32 size = 0;
        for (const StructField& f : m.structs[t.structIndex].fields)
            size += SequentialSize(m, f.type);
        return size;
    }

    inline bool CheckUniformLayout(const Module& m, Type t, StringView path, u32 std140, u32 sequential, String& error)
    {
        for (const StructField& f : m.structs[t.structIndex].fields)
        {
            String name = Format("{}.{}", path, f.name);
            if (f.type.base == BaseType::Bool)
            {
                error = Format("uniform field {} is a bool; use an int or uint, bools have no fixed layout", name);
                return false;
            }
            UniformLayout l = Std140(m, f.type);
            std140 = u32(AlignUp(std140, l.align));
            if (f.type.IsMatrix() && f.type.cols != 4)
            {
                error = Format("uniform field {} is a {}x{} matrix, which the GPU stores with padded columns; use float4x4", name, u32(f.type.cols),
                               u32(f.type.cols));
                return false;
            }
            if (std140 != sequential)
            {
                error = Format("uniform field {} is at byte {} in C# but at byte {} on the GPU; reorder the fields or add padding before it", name,
                               sequential, std140);
                return false;
            }
            if (f.type.base == BaseType::Struct && !CheckUniformLayout(m, f.type, name, std140, sequential, error))
                return false;
            std140 += l.size;
            sequential += SequentialSize(m, f.type);
        }
        return true;
    }
}
