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
            case ElementType::ValueType: case ElementType::GenericInst: break;
            default: return false;
            }
            StringView ns, name;
            if (!a.TypeName(sig.token, ns, name) || ns != kLibrary)
                return false;
            if (sig.kind == ElementType::GenericInst)
            {
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
                    {
                        out = Type::Scalar(t.base);
                        return true;
                    }
                return false;
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
                bool texture = p.type.IsTexture();
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

            // Render state of a pass or SubShader: what its attributes set, the rest from `base` (the SubShader of a pass).
            bool State(const ShaderDescription& shader, StringView owner, const Array<Attribute>& attributes, const Attribute& attribute,
                       const RenderState& base, RenderState& s)
            {
                s = base;
                s.cull = CullMode(attribute.Integer("Cull", i64(base.cull)));
                s.zWrite = attribute.Integer("ZWrite", base.zWrite ? 1 : 0) != 0;
                s.zTest = CompareFunction(attribute.Integer("ZTest", i64(base.zTest)));
                s.zClip = attribute.Integer("ZClip", base.zClip ? 1 : 0) != 0;
                s.offsetFactor = attribute.Number("OffsetFactor", base.offsetFactor);
                s.offsetUnits = attribute.Number("OffsetUnits", base.offsetUnits);
                s.colorMask = u8(attribute.Integer("ColorMask", base.colorMask) & 15);
                s.alphaToMask = attribute.Integer("AlphaToMask", base.alphaToMask ? 1 : 0) != 0;
                s.conservative = attribute.Integer("Conservative", base.conservative ? 1 : 0) != 0;
                // Blending and stencil of its own replace those of the SubShader as a whole.
                bool ownBlend = false, ownStencil = false;
                for (const Attribute& b : attributes)
                {
                    if (b.name == "BlendAttribute" && !ownBlend)
                    {
                        s.blend.Clear();
                        ownBlend = true;
                    }
                    if (b.name == "StencilAttribute" && !ownStencil)
                    {
                        RenderState none;
                        s.stencil = false;
                        s.stencilReference = none.stencilReference;
                        s.stencilReadMask = none.stencilReadMask;
                        s.stencilWriteMask = none.stencilWriteMask;
                        s.stencilFront = none.stencilFront;
                        s.stencilBack = none.stencilBack;
                        ownStencil = true;
                    }
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
                                return Fail(shader, Format("{} sets the blending of one target twice", owner));
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
                return true;
            }

            bool Pass(ShaderDescription& shader, u32 typeRow, const Array<Attribute>& attributes, const Attribute& attribute, const RenderState& base,
                      u32 subShader)
            {
                ShaderPass pass;
                pass.type = typeRow;
                pass.subShader = subShader;
                pass.assembly = &a;
                pass.name = attribute.fixed.Empty() ? StringView() : attribute.fixed[0].text;
                pass.lightMode = attribute.Text("LightMode");
                Tags(attributes, pass.tags);
                if (!State(shader, Format("pass {}", pass.name), attributes, attribute, base, pass.state))
                    return false;
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
                for (const ShaderPass& other : shader.passes)
                    if (other.subShader == subShader && other.name == pass.name)
                        return Fail(shader, Format("pass {} is declared twice", pass.name));
                shader.passes.Push(Move(pass));
                return true;
            }

            // The pass classes nested in a SubShader class (or in the definition), in source order.
            bool Passes(ShaderDescription& shader, u32 ownerRow, const RenderState& base, u32 subShader)
            {
                // Nested types follow their enclosing type in the table, in source order.
                for (u32 nested = 1; nested <= a.RowCount(Table::TypeDef); nested++)
                {
                    if (a.TypeDef(nested).enclosingType != ownerRow)
                        continue;
                    Array<Attribute> onType = Attributes(a, Token(Table::TypeDef, nested));
                    if (const Attribute* pass = Find(onType, "PassAttribute"); pass && !Pass(shader, nested, onType, *pass, base, subShader))
                        return false;
                    if (const Attribute* use = Find(onType, "UsePassAttribute"))
                    {
                        // A placeholder until ResolveUsePasses copies the pass in.
                        StringView path = use->fixed.Empty() ? StringView() : use->fixed[0].text;
                        usize slash = 0;
                        for (usize i = 0; i < path.Size(); i++)
                            if (path[i] == '/')
                                slash = i;
                        if (slash == 0 || slash + 1 >= path.Size())
                            return Fail(shader, Format("UsePass {} must name a shader and a pass, \"Shader/PASS\"", path));
                        ShaderPass pass;
                        pass.type = nested;
                        pass.subShader = subShader;
                        pass.assembly = &a;
                        pass.usePass = path;
                        pass.name = path.Substr(slash + 1);
                        shader.passes.Push(Move(pass));
                    }
                }
                return true;
            }

            bool Compute(u32 typeRow, const Attribute& attribute, ComputeDescription& out)
            {
                TypeDefInfo type = a.TypeDef(typeRow);
                // Messages and keyword checks are shared with shader definitions.
                ShaderDescription shader;
                shader.name = attribute.fixed.Empty() ? type.name : attribute.fixed[0].text;
                out.name = shader.name;
                out.type = typeRow;
                out.assembly = &a;
                for (u32 f = type.fieldBegin; f < type.fieldEnd; f++)
                {
                    FieldInfo field = a.Field(f);
                    Array<Attribute> onField = Attributes(a, Token(Table::Field, f));
                    if (const Attribute* keyword = Find(onField, "KeywordAttribute"))
                    {
                        if (!Keyword(shader, field, *keyword))
                            return false;
                    }
                    else if (!field.IsStatic() && (field.flags & 0x7) == 0x6)
                        return Fail(shader, Format("field {}: compute shaders have no material properties; use a [Uniform] block", field.name));
                }
                out.keywords = Move(shader.keywords);
                for (u32 m = type.methodBegin; m < type.methodEnd; m++)
                {
                    Array<Attribute> onMethod = Attributes(a, Token(Table::MethodDef, m));
                    const Attribute* kernel = Find(onMethod, "ComputeShaderAttribute");
                    if (!kernel)
                        continue;
                    ComputeKernel k;
                    k.name = a.Method(m).name;
                    k.method = m;
                    for (usize i = 0; i < 3 && i < kernel->fixed.Size(); i++)
                    {
                        if (kernel->fixed[i].integer < 1)
                            return Fail(shader, Format("kernel {} has a thread group size below 1", k.name));
                        k.groupSize[i] = u32(kernel->fixed[i].integer);
                    }
                    if (out.FindKernel(k.name) >= 0)
                        return Fail(shader, Format("two kernels are named {}", k.name));
                    out.kernels.Push(Move(k));
                }
                if (out.kernels.Empty())
                    return Fail(shader, "it has no kernel; mark static methods with [ComputeShader(x, y, z)]");
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
                // SubShader classes, or the passes of the definition as one SubShader.
                bool direct = false;
                for (u32 nested = 1; nested <= a.RowCount(Table::TypeDef); nested++)
                {
                    if (a.TypeDef(nested).enclosingType != typeRow)
                        continue;
                    Array<Attribute> onType = Attributes(a, Token(Table::TypeDef, nested));
                    direct = direct || Find(onType, "PassAttribute") != nullptr || Find(onType, "UsePassAttribute") != nullptr;
                    const Attribute* sub = Find(onType, "SubShaderAttribute");
                    if (!sub)
                        continue;
                    ShaderSubShader s;
                    s.type = nested;
                    s.lod = i32(sub->Integer("LOD", 0));
                    Tags(onType, s.tags);
                    RenderState state;
                    if (!State(shader, Format("SubShader {}", a.TypeDef(nested).name), onType, *sub, RenderState(), state))
                        return false;
                    shader.subShaders.Push(Move(s));
                    if (!Passes(shader, nested, state, u32(shader.subShaders.Size() - 1)))
                        return false;
                }
                if (direct && !shader.subShaders.Empty())
                    return Fail(shader, "it has passes next to SubShader classes; put every pass into a SubShader");
                if (shader.subShaders.Empty())
                {
                    ShaderSubShader s;
                    s.lod = shader.lod;
                    s.tags = shader.tags;
                    shader.subShaders.Push(Move(s));
                    if (!Passes(shader, typeRow, RenderState(), 0))
                        return false;
                }
                for (const ShaderProperty& p : shader.properties)
                    if (!p.keyword.Empty() && !shader.FindKeyword(p.keyword))
                        return Fail(shader, Format("property {} names the keyword {}, which the shader does not have", p.name, p.keyword));
                return true;
            }
        };
    }

    namespace
    {
        bool SameWithoutCase(StringView x, StringView y)
        {
            if (x.Size() != y.Size())
                return false;
            for (usize i = 0; i < x.Size(); i++)
            {
                char p = x[i] >= 'a' && x[i] <= 'z' ? char(x[i] - 32) : x[i];
                char q = y[i] >= 'a' && y[i] <= 'z' ? char(y[i] - 32) : y[i];
                if (p != q)
                    return false;
            }
            return true;
        }

        // Resolves pass `p` of shader `s`; `chain` holds the UsePass paths being resolved, to find loops.
        bool ResolvePass(Array<ShaderDescription>& shaders, usize s, usize p, Array<String>& chain, String& error)
        {
            ShaderPass& use = shaders[s].passes[p];
            if (use.usePass.Empty())
                return true;
            for (const String& c : chain)
                if (c == use.usePass)
                {
                    error = Format("shader {}: UsePass {} comes back to itself", shaders[s].name, use.usePass);
                    return false;
                }
            StringView path = use.usePass;
            usize slash = 0;
            for (usize i = 0; i < path.Size(); i++)
                if (path[i] == '/')
                    slash = i;
            StringView shaderName = path.Substr(0, slash), passName = path.Substr(slash + 1);
            for (usize t = 0; t < shaders.Size(); t++)
            {
                if (shaders[t].name != shaderName)
                    continue;
                // The first SubShader that has the pass gives it.
                for (u32 sub = 0; sub < shaders[t].subShaders.Size(); sub++)
                    for (usize q = 0; q < shaders[t].passes.Size(); q++)
                    {
                        if (shaders[t].passes[q].subShader != sub || !SameWithoutCase(shaders[t].passes[q].name, passName))
                            continue;
                        chain.Push(String(path));
                        bool ok = ResolvePass(shaders, t, q, chain, error);
                        chain.Pop();
                        if (!ok)
                            return false;
                        ShaderPass copy = shaders[t].passes[q];
                        ShaderPass& target = shaders[s].passes[p];
                        copy.subShader = target.subShader;
                        target = Move(copy);
                        return true;
                    }
                error = Format("shader {}: UsePass {}: shader {} has no pass {}", shaders[s].name, path, shaderName, passName);
                return false;
            }
            error = Format("shader {}: UsePass {}: there is no shader {}", shaders[s].name, path, shaderName);
            return false;
        }
    }

    bool ResolveUsePasses(Array<ShaderDescription>& shaders, String& error)
    {
        error.Clear();
        Array<String> chain;
        for (usize s = 0; s < shaders.Size(); s++)
            for (usize p = 0; p < shaders[s].passes.Size(); p++)
                if (!ResolvePass(shaders, s, p, chain, error))
                    return false;
        return true;
    }

    i32 ComputeDescription::FindKernel(StringView kernelName) const
    {
        for (usize i = 0; i < kernels.Size(); i++)
            if (kernels[i].name == kernelName)
                return i32(i);
        return -1;
    }

    const ShaderKeyword* ComputeDescription::FindKeyword(StringView keywordName) const
    {
        for (const ShaderKeyword& k : keywords)
            if (k.name == keywordName)
                return &k;
        return nullptr;
    }

    bool ReadComputeDefinition(const Assembly& assembly, u32 typeRow, ComputeDescription& out, String& error)
    {
        out = ComputeDescription();
        error.Clear();
        Array<Attribute> attributes = Attributes(assembly, Token(Table::TypeDef, typeRow));
        const Attribute* compute = Find(attributes, "ComputeDefinitionAttribute");
        if (!compute)
        {
            error = "the class has no [ComputeDefinition] attribute";
            return false;
        }
        Reader reader{assembly, error};
        return reader.Compute(typeRow, *compute, out);
    }

    bool ReadComputeDefinitions(const Assembly& assembly, Array<ComputeDescription>& out, String& error)
    {
        out.Clear();
        error.Clear();
        Reader reader{assembly, error};
        for (u32 row = 1; row <= assembly.RowCount(Table::TypeDef); row++)
        {
            Array<Attribute> attributes = Attributes(assembly, Token(Table::TypeDef, row));
            const Attribute* compute = Find(attributes, "ComputeDefinitionAttribute");
            if (!compute)
                continue;
            ComputeDescription description;
            if (!reader.Compute(row, *compute, description))
                return false;
            for (const ComputeDescription& other : out)
                if (other.name == description.name)
                {
                    error = Format("compute shader {} is defined twice", description.name);
                    return false;
                }
            out.Push(Move(description));
        }
        return true;
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

    bool ReadShaderDefinitions(const Assembly& assembly, Array<ShaderDescription>& out, String& error, Span<const Assembly* const> references)
    {
        out.Clear();
        error.Clear();
        Array<const Assembly*> all;
        all.Push(&assembly);
        for (const Assembly* r : references)
            if (r && r != &assembly)
                all.Push(r);
        for (const Assembly* x : all)
        {
            Reader reader{*x, error};
            for (u32 row = 1; row <= x->RowCount(Table::TypeDef); row++)
            {
                Array<Attribute> attributes = Attributes(*x, Token(Table::TypeDef, row));
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
        }
        return ResolveUsePasses(out, error);
    }
}
