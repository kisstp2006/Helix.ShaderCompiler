#include <Helix/ShaderCompiler/Definition/ShaderDefinition.h>

#include <Helix/Core/Strings/Format.h>
#include <Helix/ShaderCompiler/Assembly/Signature.h>

#include <cstring>

namespace helix::shadercompiler
{
    namespace
    {
        constexpr StringView kLibrary = "HelixEngine.Shaders";

        struct Attribute
        {
            StringView name;
            Array<AttributeArgument> fixed, named;

            const AttributeArgument* Named(StringView key) const
            {
                for (const AttributeArgument& a : named)
                    if (a.name == key)
                        return &a;
                return nullptr;
            }
            i64 Integer(StringView key, i64 fallback) const { return Named(key) ? Named(key)->integer : fallback; }
            f32 Number(StringView key, f32 fallback) const { return Named(key) ? f32(Named(key)->number) : fallback; }
            StringView Text(StringView key) const { return Named(key) ? Named(key)->text : StringView(); }
        };

        // The shader library attributes on a type, field or method, decoded.
        Array<Attribute> Attributes(const Assembly& a, Token parent)
        {
            Array<CustomAttribute> raw;
            a.Attributes(parent, raw);
            Array<Attribute> out;
            for (const CustomAttribute& c : raw)
            {
                if (c.typeNamespace != kLibrary)
                    continue;
                Attribute decoded;
                decoded.name = c.typeName;
                if (DecodeAttribute(a, c, decoded.fixed, decoded.named))
                    out.Push(Move(decoded));
            }
            return out;
        }

        const Attribute* Find(const Array<Attribute>& attributes, StringView name)
        {
            for (const Attribute& a : attributes)
                if (a.name == name)
                    return &a;
            return nullptr;
        }

        bool IsEnum(const Assembly& a, u32 typeRow)
        {
            StringView ns, name;
            return a.TypeName(a.TypeDef(typeRow).extends, ns, name) && ns == "System" && name == "Enum";
        }

        // Scalars, vectors, matrices and textures: what a material can hold.
        bool PropertyType(const Assembly& a, const TypeSig& sig, Type& out)
        {
            switch (sig.kind)
            {
            case ElementType::Boolean: out = kBool; return true;
            case ElementType::I4: out = kInt; return true;
            case ElementType::U4: out = kUInt; return true;
            case ElementType::R4: out = kFloat; return true;
            case ElementType::ValueType: break;
            default: return false;
            }
            StringView ns, name;
            if (!a.TypeName(sig.token, ns, name) || ns != kLibrary)
                return false;
            if (name == "Texture2D")
            {
                out = Type::Scalar(BaseType::Texture2D);
                return true;
            }
            static constexpr struct
            {
                StringView prefix;
                BaseType base;
            } kBases[] = {{"float", BaseType::Float}, {"int", BaseType::Int}, {"uint", BaseType::UInt}, {"bool", BaseType::Bool}};
            for (const auto& b : kBases)
            {
                if (!name.StartsWith(b.prefix))
                    continue;
                StringView rest = name.Substr(b.prefix.Size());
                if (rest.Size() == 1 && rest[0] >= '2' && rest[0] <= '4')
                {
                    out = Type::Vector(b.base, u32(rest[0] - '0'));
                    return true;
                }
                if (b.base == BaseType::Float && rest.Size() == 3 && rest[1] == 'x' && rest[0] == rest[2] && rest[0] >= '2' && rest[0] <= '4')
                {
                    out = Type::Matrix(u32(rest[0] - '0'));
                    return true;
                }
            }
            return false;
        }

        // Members of an enum with their values, from the constants of its literal fields.
        void EnumMembers(const Assembly& a, u32 typeRow, ShaderKeyword& out)
        {
            TypeDefInfo type = a.TypeDef(typeRow);
            for (u32 f = type.fieldBegin; f < type.fieldEnd; f++)
            {
                FieldInfo field = a.Field(f);
                if (!field.IsLiteral())
                    continue;
                for (u32 c = 1; c <= a.RowCount(Table::Constant); c++)
                {
                    if (a.CodedCell(Table::Constant, c, 1) != Token(Table::Field, f))
                        continue;
                    Span<const u8> blob = a.GetBlob(a.Cell(Table::Constant, c, 2));
                    // Enum values are 1 to 8 bytes, little endian; the element type in column 0 tells the sign.
                    u32 kind = a.Cell(Table::Constant, c, 0) & 0xFF;
                    bool isSigned = kind == 0x04 || kind == 0x06 || kind == 0x08 || kind == 0x0A;
                    u64 raw = 0;
                    usize size = blob.Size() < 8 ? blob.Size() : 8;
                    std::memcpy(&raw, blob.Data(), size);
                    if (isSigned && size > 0 && size < 8 && (raw >> (size * 8 - 1)) & 1)
                        raw |= ~u64(0) << (size * 8);
                    out.valueNames.Push(String(field.name));
                    out.values.Push(i64(raw));
                    break;
                }
            }
        }

        void Tags(const Array<Attribute>& attributes, Array<ShaderTag>& out)
        {
            for (const Attribute& a : attributes)
                if (a.name == "TagAttribute" && a.fixed.Size() == 2)
                    out.Push({String(a.fixed[0].text), String(a.fixed[1].text)});
        }

        struct Reader
        {
            const Assembly& a;
            String& error;

            bool Fail(const ShaderDescription& shader, StringView message)
            {
                if (error.Empty())
                    error = Format("shader {}: {}", shader.name, message);
                return false;
            }

            bool Property(ShaderDescription& shader, const FieldInfo& field, const Array<Attribute>& attributes)
            {
                ShaderProperty p;
                p.name = field.name;
                const Attribute* property = Find(attributes, "PropertyAttribute");
                p.displayName = property && !property->fixed.Empty() ? String(property->fixed[0].text) : String(field.name);
                TypeSig sig;
                if (!ParseFieldSignature(field.signature, sig) || !PropertyType(a, sig, p.type))
                    return Fail(shader, Format("property {} has a type materials cannot hold; use a number, a vector, a matrix or Texture2D", field.name));
                if (shader.FindProperty(p.name))
                    return Fail(shader, Format("property {} is declared twice", field.name));
                bool texture = p.type.base == BaseType::Texture2D;
                if (const Attribute* range = Find(attributes, "RangeAttribute"); range && range->fixed.Size() == 2)
                {
                    if (!p.type.IsScalar() || texture)
                        return Fail(shader, Format("property {} has a range but is not a number", field.name));
                    p.hasRange = true;
                    p.rangeMin = f32(range->fixed[0].number);
                    p.rangeMax = f32(range->fixed[1].number);
                }
                if (const Attribute* value = Find(attributes, "DefaultAttribute"))
                {
                    if (texture)
                        return Fail(shader, Format("texture property {} takes [DefaultTexture], not [Default]", field.name));
                    for (usize i = 0; i < value->fixed.Size() && i < 4; i++)
                        p.defaultValue[i] = f32(value->fixed[i].number);
                }
                if (const Attribute* value = Find(attributes, "DefaultTextureAttribute"); value && !value->fixed.Empty())
                {
                    if (!texture)
                        return Fail(shader, Format("property {} has a default texture but is not a texture", field.name));
                    p.defaultTexture = value->fixed[0].text;
                }
                static constexpr struct
                {
                    StringView attribute;
                    PropertyFlags flag;
                } kFlags[] = {
                    {"HDRAttribute", PropertyFlags::Hdr},
                    {"NoScaleOffsetAttribute", PropertyFlags::NoScaleOffset},
                    {"MainTextureAttribute", PropertyFlags::MainTexture},
                    {"MainColorAttribute", PropertyFlags::MainColor},
                    {"PerRendererDataAttribute", PropertyFlags::PerRendererData},
                    {"HideInInspectorAttribute", PropertyFlags::HideInInspector},
                    {"ToggleAttribute", PropertyFlags::Toggle},
                    {"KeywordEnumAttribute", PropertyFlags::KeywordEnum},
                };
                for (const auto& f : kFlags)
                    if (const Attribute* found = Find(attributes, f.attribute))
                    {
                        p.flags |= u32(f.flag);
                        if ((f.flag == PropertyFlags::Toggle || f.flag == PropertyFlags::KeywordEnum) && !found->fixed.Empty())
                            p.keyword = found->fixed[0].text;
                    }
                shader.properties.Push(Move(p));
                return true;
            }

            bool Keyword(ShaderDescription& shader, const FieldInfo& field, const Attribute& attribute)
            {
                ShaderKeyword k;
                k.name = field.name;
                // The constructor argument is the scope: 0 global, 1 local.
                k.global = !attribute.fixed.Empty() && attribute.fixed[0].integer == 0;
                k.shaderFeature = attribute.Integer("ShaderFeature", 0) != 0;
                k.dynamic = attribute.Integer("Dynamic", 0) != 0;
                TypeSig sig;
                bool isBool = ParseFieldSignature(field.signature, sig) && sig.kind == ElementType::Boolean;
                bool isEnum = sig.kind == ElementType::ValueType && sig.token.Is(Table::TypeDef) && IsEnum(a, sig.token.Row());
                if (!field.IsStatic() || (!isBool && !isEnum))
                    return Fail(shader, Format("keyword {} must be a static bool or enum field", field.name));
                if (isEnum)
                {
                    EnumMembers(a, sig.token.Row(), k);
                    if (k.values.Empty())
                        return Fail(shader, Format("keyword {} is an enum without members", field.name));
                }
                if (shader.FindKeyword(k.name))
                    return Fail(shader, Format("keyword {} is declared twice", field.name));
                shader.keywords.Push(Move(k));
                return true;
            }

            bool Pass(ShaderDescription& shader, u32 typeRow, const Array<Attribute>& attributes, const Attribute& attribute)
            {
                ShaderPass pass;
                pass.type = typeRow;
                pass.name = attribute.fixed.Empty() ? StringView() : attribute.fixed[0].text;
                pass.lightMode = attribute.Text("LightMode");
                Tags(attributes, pass.tags);
                RenderState& s = pass.state;
                s.cull = CullMode(attribute.Integer("Cull", i64(CullMode::Back)));
                s.zWrite = attribute.Integer("ZWrite", 1) != 0;
                s.zTest = CompareFunction(attribute.Integer("ZTest", i64(CompareFunction::LessEqual)));
                s.zClip = attribute.Integer("ZClip", 1) != 0;
                s.offsetFactor = attribute.Number("OffsetFactor", 0);
                s.offsetUnits = attribute.Number("OffsetUnits", 0);
                s.colorMask = u8(attribute.Integer("ColorMask", 15) & 15);
                s.alphaToMask = attribute.Integer("AlphaToMask", 0) != 0;
                s.conservative = attribute.Integer("Conservative", 0) != 0;
                for (const Attribute& b : attributes)
                {
                    if (b.name == "BlendAttribute" && (b.fixed.Size() == 2 || b.fixed.Size() == 4))
                    {
                        BlendState blend;
                        blend.target = i32(b.Integer("Target", -1));
                        blend.source = BlendFactor(b.fixed[0].integer);
                        blend.destination = BlendFactor(b.fixed[1].integer);
                        blend.sourceAlpha = b.fixed.Size() == 4 ? BlendFactor(b.fixed[2].integer) : blend.source;
                        blend.destinationAlpha = b.fixed.Size() == 4 ? BlendFactor(b.fixed[3].integer) : blend.destination;
                        blend.operation = BlendOperation(b.Integer("Operation", 0));
                        blend.operationAlpha = BlendOperation(b.Integer("OperationAlpha", 0));
                        for (const BlendState& other : s.blend)
                            if (other.target == blend.target)
                                return Fail(shader, Format("pass {} sets the blending of one target twice", pass.name));
                        s.blend.Push(blend);
                    }
                    else if (b.name == "StencilAttribute")
                    {
                        s.stencil = true;
                        s.stencilReference = u8(b.Integer("Reference", s.stencilReference));
                        s.stencilReadMask = u8(b.Integer("ReadMask", s.stencilReadMask));
                        s.stencilWriteMask = u8(b.Integer("WriteMask", s.stencilWriteMask));
                        StencilFaceState face;
                        face.compare = CompareFunction(b.Integer("Compare", i64(CompareFunction::Always)));
                        face.pass = StencilOperation(b.Integer("Pass", 0));
                        face.fail = StencilOperation(b.Integer("Fail", 0));
                        face.zFail = StencilOperation(b.Integer("ZFail", 0));
                        // Face: 0 both, 1 front, 2 back.
                        i64 which = b.Integer("Face", 0);
                        if (which != 2)
                            s.stencilFront = face;
                        if (which != 1)
                            s.stencilBack = face;
                    }
                }
                TypeDefInfo type = a.TypeDef(typeRow);
                for (u32 m = type.methodBegin; m < type.methodEnd; m++)
                {
                    Array<Attribute> onMethod = Attributes(a, Token(Table::MethodDef, m));
                    bool vertex = Find(onMethod, "VertexShaderAttribute") != nullptr, fragment = Find(onMethod, "FragmentShaderAttribute") != nullptr;
                    if ((vertex && pass.vertex) || (fragment && pass.fragment))
                        return Fail(shader, Format("pass {} has two {} shaders", pass.name, vertex ? "vertex" : "fragment"));
                    if (vertex)
                        pass.vertex = m;
                    if (fragment)
                        pass.fragment = m;
                }
                if (pass.name.Empty())
                    return Fail(shader, Format("the pass class {} has no name", type.name));
                if (!pass.vertex || !pass.fragment)
                    return Fail(shader, Format("pass {} needs a [VertexShader] and a [FragmentShader] method", pass.name));
                if (shader.FindPass(pass.name))
                    return Fail(shader, Format("pass {} is declared twice", pass.name));
                shader.passes.Push(Move(pass));
                return true;
            }

            bool Shader(u32 typeRow, const Array<Attribute>& attributes, const Attribute& attribute, ShaderDescription& shader)
            {
                TypeDefInfo type = a.TypeDef(typeRow);
                shader.type = typeRow;
                shader.name = attribute.fixed.Empty() ? type.name : attribute.fixed[0].text;
                shader.fallback = attribute.Text("Fallback");
                shader.customEditor = attribute.Text("CustomEditor");
                shader.lod = i32(attribute.Integer("LOD", 0));
                Tags(attributes, shader.tags);
                for (u32 f = type.fieldBegin; f < type.fieldEnd; f++)
                {
                    FieldInfo field = a.Field(f);
                    Array<Attribute> onField = Attributes(a, Token(Table::Field, f));
                    if (const Attribute* keyword = Find(onField, "KeywordAttribute"))
                    {
                        if (!Keyword(shader, field, *keyword))
                            return false;
                    }
                    // Every public instance field is a property; compiler generated and static fields are not.
                    else if (!field.IsStatic() && (field.flags & 0x7) == 0x6 && !Property(shader, field, onField))
                        return false;
                }
                // Nested types follow their enclosing type in the table, in source order.
                for (u32 nested = 1; nested <= a.RowCount(Table::TypeDef); nested++)
                {
                    if (a.TypeDef(nested).enclosingType != typeRow)
                        continue;
                    Array<Attribute> onType = Attributes(a, Token(Table::TypeDef, nested));
                    if (const Attribute* pass = Find(onType, "PassAttribute"); pass && !Pass(shader, nested, onType, *pass))
                        return false;
                }
                for (const ShaderProperty& p : shader.properties)
                    if (!p.keyword.Empty() && !shader.FindKeyword(p.keyword))
                        return Fail(shader, Format("property {} names the keyword {}, which the shader does not have", p.name, p.keyword));
                return true;
            }
        };
    }

    const ShaderPass* ShaderDescription::FindPass(StringView passName) const
    {
        for (const ShaderPass& p : passes)
            if (p.name == passName)
                return &p;
        return nullptr;
    }

    const ShaderProperty* ShaderDescription::FindProperty(StringView propertyName) const
    {
        for (const ShaderProperty& p : properties)
            if (p.name == propertyName)
                return &p;
        return nullptr;
    }

    const ShaderKeyword* ShaderDescription::FindKeyword(StringView keywordName) const
    {
        for (const ShaderKeyword& k : keywords)
            if (k.name == keywordName)
                return &k;
        return nullptr;
    }

    bool ReadShaderDefinition(const Assembly& assembly, u32 typeRow, ShaderDescription& out, String& error)
    {
        out = ShaderDescription();
        error.Clear();
        Array<Attribute> attributes = Attributes(assembly, Token(Table::TypeDef, typeRow));
        const Attribute* shader = Find(attributes, "ShaderAttribute");
        if (!shader)
        {
            error = "the class has no [Shader] attribute";
            return false;
        }
        Reader reader{assembly, error};
        return reader.Shader(typeRow, attributes, *shader, out);
    }

    bool ReadShaderDefinitions(const Assembly& assembly, Array<ShaderDescription>& out, String& error)
    {
        out.Clear();
        error.Clear();
        Reader reader{assembly, error};
        for (u32 row = 1; row <= assembly.RowCount(Table::TypeDef); row++)
        {
            Array<Attribute> attributes = Attributes(assembly, Token(Table::TypeDef, row));
            const Attribute* shader = Find(attributes, "ShaderAttribute");
            if (!shader)
                continue;
            ShaderDescription description;
            if (!reader.Shader(row, attributes, *shader, description))
                return false;
            for (const ShaderDescription& other : out)
                if (other.name == description.name)
                {
                    error = Format("shader {} is defined twice", description.name);
                    return false;
                }
            out.Push(Move(description));
        }
        return true;
    }
}
