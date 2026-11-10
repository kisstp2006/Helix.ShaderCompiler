#include <Helix/ShaderCompiler/Material/MaterialLayout.h>

#include <Helix/Core/Strings/Format.h>

#include <cstring>

namespace helix::shadercompiler
{
    namespace
    {
        const char* BaseName(BaseType base)
        {
            switch (base)
            {
            case BaseType::Bool: return "bool";
            case BaseType::Int: return "int";
            case BaseType::UInt: return "uint";
            case BaseType::Float: return "float";
            case BaseType::Texture2D: return "Texture2D";
            case BaseType::Texture2DArray: return "Texture2DArray";
            case BaseType::Texture3D: return "Texture3D";
            case BaseType::TextureCube: return "TextureCube";
            default: return "struct";
            }
        }

        String TypeText(Type t)
        {
            if (t.IsMatrix())
                return Format("float{}x{}", u32(t.rows), u32(t.cols));
            if (t.rows > 1)
                return Format("{}{}", BaseName(t.base), u32(t.rows));
            return String(BaseName(t.base));
        }
    }

    const PropertyBinding* MaterialLayout::Find(u32 property, PropertyTarget target) const
    {
        for (const PropertyBinding& b : bindings)
            if (b.property == property && b.target == target)
                return &b;
        return nullptr;
    }

    void WritePropertyValue(const ShaderProperty& property, const PropertyBinding& binding, Span<const f32> value, Array<u8>& blockData)
    {
        if (binding.target == PropertyTarget::Texture)
            return;
        // A scale and offset is four floats whatever the property is.
        BaseType base = binding.target == PropertyTarget::ScaleOffset ? BaseType::Float : property.type.base;
        u32 count = binding.size / 4;
        for (u32 i = 0; i < count && i < value.Size(); i++)
        {
            usize at = usize(binding.offset) + usize(i) * 4;
            if (at + 4 > blockData.Size())
                return;
            if (base == BaseType::Float)
                std::memcpy(blockData.Data() + at, &value[i], 4);
            else if (base == BaseType::Int)
            {
                i32 v = i32(value[i]);
                std::memcpy(blockData.Data() + at, &v, 4);
            }
            else
            {
                u32 v = base == BaseType::Bool ? u32(value[i] != 0.0f) : u32(value[i] < 0.0f ? 0.0f : value[i]);
                std::memcpy(blockData.Data() + at, &v, 4);
            }
        }
    }

    bool BindProperties(const ShaderDescription& shader, const PassReflection& pass, MaterialLayout& out, String& error)
    {
        out = MaterialLayout();
        error.Clear();
        // Blocks of the pass in order; the ones that take a property become material blocks.
        Array<i32> blockIndex;
        Array<u32> bound;
        blockIndex.Resize(pass.blocks.Size());
        bound.Resize(pass.blocks.Size());
        for (usize i = 0; i < pass.blocks.Size(); i++)
        {
            blockIndex[i] = -1;
            bound[i] = 0;
        }
        auto materialBlock = [&](usize passBlock) {
            if (blockIndex[passBlock] < 0)
            {
                const ReflectedBlock& source = pass.blocks[passBlock];
                MaterialBlock block;
                block.name = source.name;
                block.slot = source.slot;
                block.defaults.Resize(source.size);
                std::memset(block.defaults.Data(), 0, block.defaults.Size());
                out.blocks.Push(Move(block));
                blockIndex[passBlock] = i32(out.blocks.Size() - 1);
            }
            return u32(blockIndex[passBlock]);
        };

        for (u32 p = 0; p < shader.properties.Size(); p++)
        {
            const ShaderProperty& property = shader.properties[p];
            bool texture = property.type.IsTexture(), used = false;
            String tiling = Format("{}_ST", property.name);
            for (usize b = 0; b < pass.blocks.Size(); b++)
            {
                const ReflectedBlock& block = pass.blocks[b];
                if (const ReflectedMember* member = block.Find(property.name))
                {
                    if (texture || member->type != property.type)
                    {
                        error = Format("shader {}: property {} is a {} but the member of that name in the uniform block {} is a {}", shader.name,
                                       property.name, TypeText(property.type), block.name, TypeText(member->type));
                        return false;
                    }
                    PropertyBinding binding{p, PropertyTarget::Member, materialBlock(b), member->offset, member->size, 0};
                    WritePropertyValue(property, binding, Span<const f32>(property.defaultValue, 4), out.blocks[binding.block].defaults);
                    out.bindings.Push(binding);
                    bound[b]++;
                    used = true;
                }
                const ReflectedMember* scale = texture && !property.Has(PropertyFlags::NoScaleOffset) ? block.Find(tiling) : nullptr;
                if (scale)
                {
                    if (scale->type != Type::Vector(BaseType::Float, 4))
                    {
                        error = Format("shader {}: {} in the uniform block {} must be a float4 (scale and offset of the texture {})", shader.name, tiling,
                                       block.name, property.name);
                        return false;
                    }
                    PropertyBinding binding{p, PropertyTarget::ScaleOffset, materialBlock(b), scale->offset, scale->size, 0};
                    const f32 identity[4] = {1.0f, 1.0f, 0.0f, 0.0f};
                    WritePropertyValue(property, binding, identity, out.blocks[binding.block].defaults);
                    out.bindings.Push(binding);
                    bound[b]++;
                    used = true;
                }
            }
            for (const ReflectedResource& r : pass.resources)
            {
                if (r.name != property.name)
                    continue;
                if (!texture || r.kind != ResourceKind::Texture)
                {
                    error = Format("shader {}: property {} is a {} but the pass has a {} of that name", shader.name, property.name,
                                   TypeText(property.type), r.kind == ResourceKind::Texture ? "texture" : "storage resource");
                    return false;
                }
                PropertyBinding binding;
                binding.property = p;
                binding.target = PropertyTarget::Texture;
                binding.slot = r.slot;
                out.bindings.Push(binding);
                used = true;
            }
            if (!used)
                out.unused.Push(p);
        }
        for (usize b = 0; b < pass.blocks.Size(); b++)
            if (blockIndex[b] >= 0)
                out.blocks[usize(blockIndex[b])].propertiesOnly = bound[b] == pass.blocks[b].members.Size();
        return true;
    }
}
