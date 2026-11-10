#include <Helix/ShaderCompiler/Reflection/Reflection.h>

#include <Helix/Core/Strings/Format.h>
#include <Helix/ShaderCompiler/Backends/Implementation/UniformLayout.h>

namespace helix::shadercompiler
{
    namespace
    {
        // Lists the members of a uniform struct; the layout was checked, so offsets follow the fields in order.
        void Members(const Module& m, Type t, StringView path, u32& offset, Array<ReflectedMember>& out)
        {
            for (const StructField& f : m.structs[t.structIndex].fields)
            {
                String name = path.Empty() ? String(f.name) : Format("{}.{}", path, f.name);
                if (f.type.base == BaseType::Struct)
                {
                    Members(m, f.type, name, offset, out);
                    continue;
                }
                u32 size = SequentialSize(m, f.type);
                out.Push({Move(name), f.type, offset, size});
                offset += size;
            }
        }

        bool Value(Semantic semantic, u32 index, Type type, bool flat, StringView name, StringView what, Array<ReflectedValue>& out, String& error)
        {
            if (semantic == Semantic::None)
            {
                error = Format("{} has no stage attribute such as [Location(n)] or [Position]", what);
                return false;
            }
            // Integers are never interpolated.
            out.Push({String(name), semantic, index, type, flat || type.IsInteger()});
            return true;
        }

        template <typename T>
        const T* FindSlot(const Array<T>& items, u32 slot)
        {
            for (const T& item : items)
                if (item.slot == slot)
                    return &item;
            return nullptr;
        }

        bool SameBlock(const ReflectedBlock& a, const ReflectedBlock& b)
        {
            if (a.size != b.size || a.members.Size() != b.members.Size())
                return false;
            for (usize i = 0; i < a.members.Size(); i++)
                if (a.members[i].name != b.members[i].name || a.members[i].type != b.members[i].type || a.members[i].offset != b.members[i].offset)
                    return false;
            return true;
        }

        // Textures have their own slots; storage buffers and storage textures share one set.
        u32 SlotSpace(ResourceKind kind) { return kind == ResourceKind::Texture ? 0u : 1u; }

    }

    const ReflectedMember* ReflectedBlock::Find(StringView memberName) const
    {
        for (const ReflectedMember& m : members)
            if (m.name == memberName)
                return &m;
        return nullptr;
    }

    const ReflectedBlock* ShaderReflection::FindBlock(StringView name) const
    {
        for (const ReflectedBlock& b : blocks)
            if (b.name == name)
                return &b;
        return nullptr;
    }

    const ReflectedResource* ShaderReflection::FindResource(StringView name) const
    {
        for (const ReflectedResource& r : resources)
            if (r.name == name)
                return &r;
        return nullptr;
    }

    bool Reflect(const Module& m, ShaderReflection& out, String& error)
    {
        out = ShaderReflection();
        error.Clear();
        out.stage = m.stage;
        out.keywords = m.keywords;
        out.bindless = FindBindless(m);
        for (int i = 0; i < 3; i++)
            out.groupSize[i] = m.groupSize[i];

        for (const Global& g : m.globals)
        {
            if (g.kind == GlobalKind::Uniform)
            {
                if (g.type.base != BaseType::Struct)
                {
                    error = Format("uniform {} must be a struct", g.name);
                    return false;
                }
                if (!CheckUniformLayout(m, g.type, g.name, 0, 0, error))
                    return false;
                ReflectedBlock block;
                block.name = g.name;
                block.slot = g.slot;
                u32 offset = 0;
                Members(m, g.type, "", offset, block.members);
                block.size = u32(AlignUp(offset, 16));
                if (const ReflectedBlock* other = FindSlot(out.blocks, g.slot))
                {
                    error = Format("uniform blocks {} and {} both use slot {}", other->name, g.name, g.slot);
                    return false;
                }
                out.blocks.Push(Move(block));
                continue;
            }
            if (g.kind == GlobalKind::Sampler)
                continue;
            ReflectedResource r;
            r.name = g.name;
            r.slot = g.slot;
            if (g.kind == GlobalKind::Storage)
            {
                bool image = g.type.base == BaseType::Image;
                if (!image && !CheckBufferLayout(m, g.element, g.name, error))
                    return false;
                r.kind = image ? ResourceKind::StorageTexture : ResourceKind::StorageBuffer;
                r.writable = g.writable;
                r.element = g.element;
                r.stride = image ? 0 : SequentialSize(m, g.element);
                r.format = g.format;
            }
            for (const ReflectedResource& other : out.resources)
                if (other.slot == r.slot && SlotSpace(other.kind) == SlotSpace(r.kind))
                {
                    error = Format("{} and {} both use slot {}", other.name, r.name, r.slot);
                    return false;
                }
            out.resources.Push(Move(r));
        }

        const Function& entry = m.functions[m.entry];
        for (const Variable& p : entry.params)
        {
            if (p.type.base == BaseType::Struct)
            {
                for (const StructField& f : m.structs[p.type.structIndex].fields)
                    if (!Value(f.semantic, f.semanticIndex, f.type, f.flat, f.name, Format("field {} of parameter {}", f.name, p.name), out.inputs, error))
                        return false;
            }
            else if (!Value(p.semantic, p.semanticIndex, p.type, p.flat, p.name, Format("parameter {}", p.name), out.inputs, error))
                return false;
        }
        Type r = entry.returnType;
        if (r.base == BaseType::Struct)
        {
            for (const StructField& f : m.structs[r.structIndex].fields)
                if (!Value(f.semantic, f.semanticIndex, f.type, f.flat, f.name, Format("field {} of the return value", f.name), out.outputs, error))
                    return false;
        }
        else if (!r.IsVoid() && !Value(m.returnSemantic, m.returnSemanticIndex, r, false, "result", "the return value", out.outputs, error))
            return false;
        return true;
    }

    bool LinkReflection(const ShaderReflection& vertex, const ShaderReflection& fragment, PassReflection& out, String& error)
    {
        out = PassReflection();
        error.Clear();
        if (vertex.stage != Stage::Vertex || fragment.stage != Stage::Fragment)
        {
            error = "a pass links a vertex stage with a fragment stage";
            return false;
        }
        for (const ShaderReflection* stage : {&vertex, &fragment})
        {
            for (const ReflectedBlock& block : stage->blocks)
            {
                const ReflectedBlock* other = FindSlot(out.blocks, block.slot);
                if (!other)
                    out.blocks.Push(block);
                else if (!SameBlock(*other, block))
                {
                    error = Format("uniform slot {} holds {} in the vertex stage and {} in the fragment stage, and they differ", block.slot, other->name,
                                   block.name);
                    return false;
                }
            }
            for (const ReflectedResource& r : stage->resources)
            {
                const ReflectedResource* other = nullptr;
                for (const ReflectedResource& o : out.resources)
                    if (o.slot == r.slot && SlotSpace(o.kind) == SlotSpace(r.kind))
                        other = &o;
                if (!other)
                    out.resources.Push(r);
                else if (other->kind != r.kind || other->element != r.element || other->writable != r.writable || other->format != r.format)
                {
                    error = Format("slot {} holds {} in the vertex stage and {} in the fragment stage, and they differ", r.slot, other->name, r.name);
                    return false;
                }
            }
            out.bindless.textures = out.bindless.textures || stage->bindless.textures;
            out.bindless.buffers = out.bindless.buffers || stage->bindless.buffers;
            out.bindless.bufferWrites = out.bindless.bufferWrites || stage->bindless.bufferWrites;
            for (const KeywordUse& use : stage->keywords)
            {
                bool known = false;
                for (const KeywordUse& other : out.keywords)
                    known = known || other.name == use.name;
                if (!known)
                    out.keywords.Push(use);
            }
        }

        bool position = false;
        for (const ReflectedValue& v : vertex.outputs)
            position = position || v.semantic == Semantic::Position;
        if (!position)
        {
            error = "the vertex stage does not write a [Position]";
            return false;
        }
        for (const ReflectedValue& in : fragment.inputs)
        {
            if (in.semantic != Semantic::Location)
                continue;
            const ReflectedValue* written = nullptr;
            for (const ReflectedValue& o : vertex.outputs)
                if (o.semantic == Semantic::Location && o.index == in.index)
                    written = &o;
            if (!written)
            {
                error = Format("the fragment stage reads {} at location {}, which the vertex stage does not write", in.name, in.index);
                return false;
            }
            if (written->type != in.type || written->flat != in.flat)
            {
                error = Format("location {} is written as {} by the vertex stage and read as {} by the fragment stage with another type or interpolation",
                               in.index, written->name, in.name);
                return false;
            }
        }
        for (const ReflectedValue& v : vertex.inputs)
        {
            if (v.semantic != Semantic::Location)
                continue;
            for (const ReflectedValue& other : out.vertexInputs)
                if (other.index == v.index)
                {
                    error = Format("vertex inputs {} and {} both use location {}", other.name, v.name, v.index);
                    return false;
                }
            out.vertexInputs.Push(v);
        }
        for (const ReflectedValue& v : fragment.outputs)
            if (v.semantic == Semantic::Target)
                out.targets.Push(v);
        return true;
    }
}
