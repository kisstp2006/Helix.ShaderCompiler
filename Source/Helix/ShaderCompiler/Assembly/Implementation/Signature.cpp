#include <Helix/ShaderCompiler/Assembly/Signature.h>

#include <cstring>

namespace helix::shadercompiler
{
    namespace
    {
        constexpr u32 kMaxDepth = 32;
        constexpr u32 kMaxCount = 1024;

        struct Reader
        {
            Span<const u8> data;
            usize at = 0;
            bool ok = true;

            u8 Byte()
            {
                if (at >= data.Size())
                {
                    ok = false;
                    return 0;
                }
                return data[at++];
            }

            u8 Peek() const { return at < data.Size() ? data[at] : 0; }

            u32 Compressed()
            {
                u32 v = 0;
                if (!ReadCompressed(data, at, v))
                    ok = false;
                return v;
            }

            // TypeDefOrRef coded index as stored in signatures.
            Token TypeToken()
            {
                static constexpr Table kTables[3] = {Table::TypeDef, Table::TypeRef, Table::TypeSpec};
                u32 v = Compressed();
                if ((v & 3) == 3)
                {
                    ok = false;
                    return {};
                }
                return Token(kTables[v & 3], v >> 2);
            }

            template <typename T>
            T Raw()
            {
                T v{};
                if (data.Size() - at < sizeof(T))
                {
                    ok = false;
                    at = data.Size();
                    return v;
                }
                std::memcpy(&v, data.Data() + at, sizeof(T));
                at += sizeof(T);
                return v;
            }

            // Serialized string of attribute blobs: 0xFF is null, otherwise a compressed length and UTF-8 bytes.
            StringView SerString()
            {
                if (Peek() == 0xFF)
                {
                    at++;
                    return {};
                }
                u32 length = Compressed();
                if (!ok || length > data.Size() - at)
                {
                    ok = false;
                    return {};
                }
                StringView s(reinterpret_cast<const char*>(data.Data()) + at, length);
                at += length;
                return s;
            }
        };

        bool ParseType(Reader& r, TypeSig& out, u32 depth)
        {
            if (depth > kMaxDepth)
                return r.ok = false;
            for (;;)
            {
                u8 e = r.Peek();
                if (e == 0x1F || e == 0x20)
                {
                    // Custom modifiers do not change the type for our purposes.
                    r.Byte();
                    r.TypeToken();
                }
                else if (e == 0x45)
                {
                    r.Byte();
                    out.pinned = true;
                }
                else
                    break;
                if (!r.ok)
                    return false;
            }
            out.kind = ElementType(r.Byte());
            switch (out.kind)
            {
            case ElementType::Void: case ElementType::Boolean: case ElementType::Char: case ElementType::I1: case ElementType::U1:
            case ElementType::I2: case ElementType::U2: case ElementType::I4: case ElementType::U4: case ElementType::I8:
            case ElementType::U8: case ElementType::R4: case ElementType::R8: case ElementType::String: case ElementType::I:
            case ElementType::U: case ElementType::Object: case ElementType::TypedByRef:
                break;
            case ElementType::ValueType:
            case ElementType::Class:
                out.token = r.TypeToken();
                break;
            case ElementType::Var:
            case ElementType::MVar:
                out.index = r.Compressed();
                break;
            case ElementType::Ptr:
            case ElementType::ByRef:
            case ElementType::SzArray:
                ParseType(r, out.args.Emplace(), depth + 1);
                break;
            case ElementType::Array:
            {
                ParseType(r, out.args.Emplace(), depth + 1);
                out.index = r.Compressed();
                for (int pass = 0; pass < 2 && r.ok; pass++)
                {
                    u32 count = r.Compressed();
                    if (count > kMaxCount)
                        return r.ok = false;
                    for (u32 i = 0; i < count && r.ok; i++)
                        r.Compressed();
                }
                break;
            }
            case ElementType::GenericInst:
            {
                u8 kind = r.Byte();
                if (kind != u8(ElementType::Class) && kind != u8(ElementType::ValueType))
                    return r.ok = false;
                out.genericValueType = kind == u8(ElementType::ValueType);
                out.token = r.TypeToken();
                u32 count = r.Compressed();
                if (count == 0 || count > kMaxCount)
                    return r.ok = false;
                for (u32 i = 0; i < count && r.ok; i++)
                    ParseType(r, out.args.Emplace(), depth + 1);
                break;
            }
            default:
                // Function pointers and unknown element types are not supported.
                return r.ok = false;
            }
            return r.ok;
        }

        bool ParseTypeList(Reader& r, u32 count, Array<TypeSig>& out)
        {
            if (count > kMaxCount)
                return r.ok = false;
            for (u32 i = 0; i < count && r.ok; i++)
                ParseType(r, out.Emplace(), 0);
            return r.ok;
        }
    }

    bool ParseMethodSignature(Span<const u8> blob, MethodSig& out)
    {
        out = MethodSig();
        Reader r{blob};
        u8 convention = r.Byte();
        out.hasThis = (convention & 0x20) != 0;
        if (convention & 0x10)
            out.genericParamCount = r.Compressed();
        u32 count = r.Compressed();
        return r.ok && ParseType(r, out.returnType, 0) && ParseTypeList(r, count, out.params);
    }

    bool ParseFieldSignature(Span<const u8> blob, TypeSig& out)
    {
        out = TypeSig();
        Reader r{blob};
        return r.Byte() == 0x06 && ParseType(r, out, 0);
    }

    bool ParseLocalsSignature(Span<const u8> blob, Array<TypeSig>& out)
    {
        out.Clear();
        Reader r{blob};
        if (r.Byte() != 0x07)
            return false;
        u32 count = r.Compressed();
        return r.ok && ParseTypeList(r, count, out);
    }

    bool ParseTypeSpec(Span<const u8> blob, TypeSig& out)
    {
        out = TypeSig();
        Reader r{blob};
        return ParseType(r, out, 0);
    }

    bool ParseMethodSpec(Span<const u8> blob, Array<TypeSig>& out)
    {
        out.Clear();
        Reader r{blob};
        if (r.Byte() != 0x0A)
            return false;
        u32 count = r.Compressed();
        return r.ok && ParseTypeList(r, count, out);
    }

    namespace
    {
        // Reads one attribute value of the given element type (0x55 is an enum, read as its usual 32 bit value).
        bool ReadAttributeValue(Reader& r, u8 type, AttributeArgument& out)
        {
            using Kind = AttributeArgument::Kind;
            switch (type)
            {
            case 0x02: out.kind = Kind::Boolean; out.integer = r.Raw<u8>() != 0; break;
            case 0x03: out.integer = r.Raw<u16>(); break;
            case 0x04: out.integer = r.Raw<i8>(); break;
            case 0x05: out.integer = r.Raw<u8>(); break;
            case 0x06: out.integer = r.Raw<i16>(); break;
            case 0x07: out.integer = r.Raw<u16>(); break;
            case 0x08: case 0x11: case 0x55: out.integer = r.Raw<i32>(); break;
            case 0x09: out.integer = r.Raw<u32>(); break;
            case 0x0A: out.integer = r.Raw<i64>(); break;
            case 0x0B: out.integer = i64(r.Raw<u64>()); break;
            case 0x0C: out.kind = Kind::Number; out.number = r.Raw<f32>(); break;
            case 0x0D: out.kind = Kind::Number; out.number = r.Raw<f64>(); break;
            case 0x0E: case 0x50: out.kind = Kind::Text; out.text = r.SerString(); break;
            default: return r.ok = false;
            }
            return r.ok;
        }
    }

    bool DecodeAttribute(const Assembly& assembly, const CustomAttribute& attribute, Array<AttributeArgument>& fixed, Array<AttributeArgument>& named)
    {
        fixed.Clear();
        named.Clear();
        Span<const u8> signature;
        if (attribute.constructor.Is(Table::MethodDef))
            signature = assembly.Method(attribute.constructor.Row()).signature;
        else if (attribute.constructor.Is(Table::MemberRef))
            signature = assembly.MemberRef(attribute.constructor.Row()).signature;
        MethodSig ctor;
        if (!ParseMethodSignature(signature, ctor))
            return false;
        Reader r{attribute.value};
        if (r.Raw<u16>() != 0x0001)
            return false;
        for (const TypeSig& p : ctor.params)
        {
            u8 type = u8(p.kind);
            // System.Type arguments are stored as type name strings.
            if (p.kind == ElementType::Class)
                type = 0x50;
            if (!ReadAttributeValue(r, type, fixed.Emplace()))
                return false;
        }
        u32 count = r.Raw<u16>();
        for (u32 i = 0; i < count; i++)
        {
            u8 target = r.Byte();
            if (target != 0x53 && target != 0x54)
                return false;
            u8 type = r.Byte();
            if (type == 0x55)
                r.SerString();
            AttributeArgument& a = named.Emplace();
            a.name = r.SerString();
            if (!r.ok || !ReadAttributeValue(r, type, a))
                return false;
        }
        return r.ok;
    }
}
