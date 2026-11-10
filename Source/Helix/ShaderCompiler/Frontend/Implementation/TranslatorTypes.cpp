#include <Helix/ShaderCompiler/Frontend/Implementation/Translator.h>

namespace helix::shadercompiler::frontend
{
    String Translator::At(u32 assembly, u32 method, u32 offset) const
    {
        String file;
        u32 line = 0;
        if (method == 0 || !symbols[assembly] || !symbols[assembly]->Find(method, offset, file, line))
            return String();
        return Format("{}:{}: ", file, line);
    }

    bool Translator::Fail(StringView method, StringView message)
    {
        if (error.Empty())
            error = Format("{}{}: {}", At(location.assembly, location.method, location.offset), method, message);
        return false;
    }

    bool Translator::Has(Token parent, StringView attribute, Array<AttributeArgument>* fixed) const
    {
        Array<CustomAttribute> attributes;
        a->Attributes(parent, attributes);
        Array<AttributeArgument> f, n;
        for (const CustomAttribute& c : attributes)
            if (c.typeNamespace == kLibrary && c.typeName == attribute)
            {
                if (fixed && !DecodeAttribute(*a, c, *fixed, n))
                    return false;
                return true;
            }
        return false;
    }

    Semantic Translator::ReadSemantic(Token parent, u32& index, bool& flat) const
    {
        static constexpr struct
        {
            StringView name;
            Semantic semantic;
        } kSemantics[] = {{"PositionAttribute", Semantic::Position},       {"LocationAttribute", Semantic::Location},
                          {"TargetAttribute", Semantic::Target},           {"DepthAttribute", Semantic::Depth},
                          {"VertexIdAttribute", Semantic::VertexId},       {"InstanceIdAttribute", Semantic::InstanceId},
                          {"FrontFacingAttribute", Semantic::FrontFacing}, {"DispatchThreadIdAttribute", Semantic::DispatchThreadId}};
        Semantic result = Semantic::None;
        index = 0;
        Array<AttributeArgument> fixed;
        for (const auto& s : kSemantics)
            if (Has(parent, s.name, &fixed))
            {
                result = s.semantic;
                index = fixed.Empty() ? 0 : u32(fixed[0].integer);
            }
        flat = Has(parent, "InterpolationAttribute", &fixed) && !fixed.Empty() && fixed[0].integer == 1;
        return result;
    }

    u32 Translator::InlineArrayLength(u32 typeRow) const
    {
        Array<CustomAttribute> attributes;
        a->Attributes(Token(Table::TypeDef, typeRow), attributes);
        for (const CustomAttribute& c : attributes)
            if (c.typeNamespace == "System.Runtime.CompilerServices" && c.typeName == "InlineArrayAttribute")
            {
                Array<AttributeArgument> fixed, named;
                if (DecodeAttribute(*a, c, fixed, named) && !fixed.Empty() && fixed[0].integer > 0)
                    return u32(fixed[0].integer);
            }
        return 0;
    }

    i32 Translator::ArrayFor(u32 typeRow, u32 length, StringView method)
    {
        if (i32 known = FindKnown(knownArrays, Key(typeRow)); known >= 0)
            return known;
        // Damaged metadata can make a type contain itself; mapping it would never end.
        if (mappingTypes.Contains(Key(typeRow)))
            return Fail(method, Format("type {} contains itself", a->TypeDef(typeRow).name)), -1;
        mappingTypes.Push(Key(typeRow));
        struct Done
        {
            Array<u64>& list;
            ~Done() { list.Pop(); }
        } done{mappingTypes};
        TypeDefInfo t = a->TypeDef(typeRow);
        ArrayType array;
        array.length = length;
        bool found = false;
        for (u32 f = t.fieldBegin; f < t.fieldEnd; f++)
        {
            FieldInfo field = a->Field(f);
            if (field.IsStatic())
                continue;
            TypeSig sig;
            bool byRef = false;
            if (found || !ParseFieldSignature(field.signature, sig) || !MapType(sig, nullptr, array.element, byRef, method))
            {
                Fail(method, Format("inline array {} must have one field of a type shaders can use", t.name));
                return -1;
            }
            found = true;
        }
        if (!found || array.element.base == BaseType::Array)
        {
            Fail(method, Format("inline array {}: {}", t.name, found ? "arrays of arrays are not available in shaders" : "it has no element field"));
            return -1;
        }
        knownArrays.Push({Key(typeRow), u32(m.arrays.Size())});
        m.arrays.Push(Move(array));
        return i32(m.arrays.Size() - 1);
    }

    i32 Translator::StructFor(u32 typeRow, StringView method)
    {
        if (i32 known = FindKnown(knownStructs, Key(typeRow)); known >= 0)
            return known;
        // Damaged metadata can make a type contain itself; mapping it would never end.
        if (mappingTypes.Contains(Key(typeRow)))
            return Fail(method, Format("type {} contains itself", a->TypeDef(typeRow).name)), -1;
        mappingTypes.Push(Key(typeRow));
        struct Done
        {
            Array<u64>& list;
            ~Done() { list.Pop(); }
        } done{mappingTypes};
        TypeDefInfo t = a->TypeDef(typeRow);
        StructType s;
        s.name = t.name;
        for (u32 f = t.fieldBegin; f < t.fieldEnd; f++)
        {
            FieldInfo field = a->Field(f);
            if (field.IsStatic())
                continue;
            TypeSig sig;
            StructField& out = s.fields.Emplace();
            bool byRef = false;
            // Nested closures refer to each other; shaders keep a closure as a value, which would copy the outer one.
            if (IsClosureType(typeRow) && ParseFieldSignature(field.signature, sig) &&
                (sig.kind == ElementType::Class || sig.kind == ElementType::Object))
            {
                Fail(method, "a lambda captures variables of two scopes (for example of a loop body and of the method) or this; "
                             "declare the variables it captures in one scope");
                return -1;
            }
            // A fixed buffer looks like a struct of one element; it is refused before its type is mapped.
            Array<CustomAttribute> attributes;
            a->Attributes(Token(Table::Field, f), attributes);
            for (const CustomAttribute& c : attributes)
                if (c.typeNamespace == "System.Runtime.CompilerServices" && c.typeName == "FixedBufferAttribute")
                {
                    Fail(method, Format("field {}.{} is a fixed buffer, which needs unsafe code; use an [InlineArray] struct", t.name, field.name));
                    return -1;
                }
            if (!ParseFieldSignature(field.signature, sig) || !MapType(sig, nullptr, out.type, byRef, method))
            {
                Fail(method, Format("field {}.{} has a type that shaders cannot use", t.name, field.name));
                return -1;
            }
            out.name = field.name;
            out.semantic = ReadSemantic(Token(Table::Field, f), out.semanticIndex, out.flat);
        }
        // Register after the fields, so nested structs come first.
        knownStructs.Push({Key(typeRow), u32(m.structs.Size())});
        m.structs.Push(Move(s));
        return i32(m.structs.Size() - 1);
    }

    bool Translator::IsEnum(u32 typeRow) const
    {
        StringView ns, name;
        return a->TypeName(a->TypeDef(typeRow).extends, ns, name) && ns == "System" && name == "Enum";
    }

    bool Translator::MapType(const TypeSig& sig, const Method* method, Type& out, bool& byRef, StringView where)
    {
        switch (sig.kind)
        {
        case ElementType::Void: out = kVoid; return true;
        case ElementType::Boolean: out = kBool; return true;
        case ElementType::I4: out = kInt; return true;
        case ElementType::U4: out = kUInt; return true;
        // Doubles are computed as floats.
        case ElementType::R4: case ElementType::R8: out = kFloat; return true;
        case ElementType::ByRef:
            byRef = true;
            return MapType(sig.args[0], method, out, byRef, where);
        case ElementType::MVar:
            if (!method || sig.index >= method->typeArgs.Size())
                return false;
            out = method->typeArgs[sig.index];
            return true;
        case ElementType::ValueType:
        {
            StringView ns, name;
            if (!a->TypeName(sig.token, ns, name))
                return false;
            if (ns == kLibrary)
                return LibraryType(name, out);
            // A struct of an included assembly is read in that assembly.
            u32 assembly = current, row = 0;
            if (!ResolveType(sig.token, assembly, row))
            {
                if (sig.token.Is(Table::TypeRef))
                    Fail(where, Format("type {}.{} comes from an assembly that is not among TranslateOptions::references", ns, name));
                return false;
            }
            return InAssembly(assembly, [&] { return MapValueType(row, out, where); });
        }
        case ElementType::Class:
        {
            // The class the C# compiler keeps variables captured by lambdas in is a struct in shaders.
            if (!sig.token.Is(Table::TypeDef) || !IsClosureType(sig.token.Row()))
                return false;
            i32 index = StructFor(sig.token.Row(), where);
            if (index < 0)
                return false;
            out = Type::Struct(u32(index));
            return true;
        }
        case ElementType::GenericInst:
            if (IsImageType(sig))
            {
                out = Type::Scalar(BaseType::Image);
                return true;
            }
            if (BaseType texture = TextureType(sig); texture != BaseType::Void)
            {
                out = Type::Scalar(texture);
                return true;
            }
            if (!IsBufferType(sig))
                return false;
            out = Type::Scalar(BaseType::Buffer);
            return true;
        default: return false;
        }
    }

    bool Translator::MapValueType(u32 row, Type& out, StringView where)
    {
        if (IsEnum(row))
        {
            out = kInt;
            return true;
        }
        if (u32 length = InlineArrayLength(row))
        {
            i32 array = ArrayFor(row, length, where);
            if (array < 0)
                return false;
            out = Type::ArrayOf(u32(array));
            return true;
        }
        i32 index = StructFor(row, where);
        if (index < 0)
            return false;
        out = Type::Struct(u32(index));
        return true;
    }

    bool Translator::IsSpanType(const TypeSig& sig) const
    {
        StringView ns, name;
        return sig.kind == ElementType::GenericInst && sig.args.Size() == 1 && a->TypeName(sig.token, ns, name) && ns == "System" &&
               (name == "Span`1" || name == "ReadOnlySpan`1");
    }

    bool Translator::IsImageType(const TypeSig& sig) const
    {
        StringView ns, name;
        return sig.kind == ElementType::GenericInst && sig.args.Size() == 1 && a->TypeName(sig.token, ns, name) && ns == kLibrary &&
               name == "RWTexture2D`1";
    }

    BaseType Translator::TextureType(const TypeSig& sig) const
    {
        StringView ns, name;
        if (sig.kind != ElementType::GenericInst || sig.args.Size() != 1 || !a->TypeName(sig.token, ns, name) || ns != kLibrary)
            return BaseType::Void;
        static constexpr struct
        {
            StringView name;
            BaseType base;
        } kTextures[] = {{"Texture2D`1", BaseType::Texture2D},
                         {"Texture2DArray`1", BaseType::Texture2DArray},
                         {"Texture3D`1", BaseType::Texture3D},
                         {"TextureCube`1", BaseType::TextureCube}};
        for (const auto& t : kTextures)
            if (name == t.name)
                return t.base;
        return BaseType::Void;
    }

    bool Translator::IsBufferType(const TypeSig& sig, bool* writable) const
    {
        StringView ns, name;
        if (sig.kind != ElementType::GenericInst || sig.args.Size() != 1 || !a->TypeName(sig.token, ns, name) || ns != kLibrary)
            return false;
        if (writable)
            *writable = name.StartsWith("RW");
        return name.StartsWith("StructuredBuffer") || name.StartsWith("RWStructuredBuffer");
    }
}
