#include <Helix/ShaderCompiler/Frontend/Implementation/Translator.h>

namespace helix::shadercompiler::frontend
{
    bool Translator::FieldExpr(Method& mt, Token token, ExprId object, ExprId& out)
    {
        StringView name, ns, typeName, member;
        // Inside the library a field of one of its types is still the vector component or matrix column.
        bool library = token.Is(Table::Field) && a->TypeName(Token(Table::TypeDef, a->DeclaringTypeOfField(token.Row())), ns, typeName) &&
                       ns == kLibrary;
        if (library)
            member = a->Field(token.Row()).name;
        else if (token.Is(Table::Field))
        {
            u32 typeRow = a->DeclaringTypeOfField(token.Row());
            i32 index = StructFor(typeRow, mt.name);
            if (index < 0)
                return false;
            name = a->Field(token.Row()).name;
            const StructType& s = m.structs[usize(index)];
            for (usize i = 0; i < s.fields.Size(); i++)
                if (StringView(s.fields[i].name) == name)
                {
                    Expr e;
                    e.kind = ExprKind::Field;
                    e.a = object;
                    e.index = u32(i);
                    e.type = s.fields[i].type;
                    out = AddExpr(mt.fn, Move(e));
                    return true;
                }
            return Fail(mt.name, Format("unknown field {}", name));
        }
        else if (token.Is(Table::MemberRef))
        {
            MemberRefInfo ref = a->MemberRef(token.Row());
            member = ref.name;
            if (!a->TypeName(ref.parent, ns, typeName))
                return Fail(mt.name, Format("field {} of a type that shaders cannot use", member));
            u32 assembly = current, typeRow = 0;
            if (ns != kLibrary && ResolveType(ref.parent, assembly, typeRow))
            {
                // A field of a struct of an included assembly.
                i32 index = InAssembly(assembly, [&] { return StructFor(typeRow, mt.name); });
                if (index < 0)
                    return false;
                const StructType& s = m.structs[usize(index)];
                for (usize i = 0; i < s.fields.Size(); i++)
                    if (StringView(s.fields[i].name) == member)
                    {
                        Expr e;
                        e.kind = ExprKind::Field;
                        e.a = object;
                        e.index = u32(i);
                        e.type = s.fields[i].type;
                        out = AddExpr(mt.fn, Move(e));
                        return true;
                    }
                return Fail(mt.name, Format("unknown field {}.{}", typeName, member));
            }
        }
        else
            return Fail(mt.name, "unsupported field reference");
        Type type;
        if (ns != kLibrary || !LibraryType(typeName, type))
            return Fail(mt.name, Format("field {} of a type that shaders cannot use", member));
        if (type.IsMatrix() && member.Size() == 2 && member[0] == 'c')
        {
            out = Index(mt, object, ConstInt(mt, member[1] - '0', kInt));
            return true;
        }
        i32 component = member.Size() == 1 ? ComponentIndex(member[0]) : -1;
        if (!type.IsVector() || component < 0)
            return Fail(mt.name, Format("unknown field {}.{}", typeName, member));
        u8 c = u8(component);
        out = Swizzle(mt, object, &c, 1);
        return true;
    }

    bool Translator::IsDynamicKeyword(u32 fieldRow) const
    {
        Array<CustomAttribute> attributes;
        a->Attributes(Token(Table::Field, fieldRow), attributes);
        for (const CustomAttribute& c : attributes)
        {
            Array<AttributeArgument> fixed, named;
            if (c.typeNamespace != kLibrary || c.typeName != "KeywordAttribute" || !DecodeAttribute(*a, c, fixed, named))
                continue;
            for (const AttributeArgument& n : named)
                if (n.name == "Dynamic")
                    return n.integer != 0;
        }
        return false;
    }

    i32 Translator::KeywordMember(Method& mt, u32 fieldRow)
    {
        if (keywordGlobal < 0)
        {
            StructType s;
            s.name = kKeywordBlockName;
            m.structs.Push(Move(s));
            Global g;
            g.name = kKeywordBlockName;
            g.kind = GlobalKind::Uniform;
            g.slot = kKeywordBlockSlot;
            g.type = Type::Struct(u32(m.structs.Size() - 1));
            m.globals.Push(Move(g));
            keywordGlobal = i32(m.globals.Size() - 1);
        }
        StructType& block = m.structs[m.globals[usize(keywordGlobal)].type.structIndex];
        u32 typeRow = a->DeclaringTypeOfField(fieldRow);
        if (!keywordClasses.Find(Key(typeRow)))
        {
            keywordClasses.Push(Key(typeRow));
            TypeDefInfo t = a->TypeDef(typeRow);
            for (u32 f = t.fieldBegin; f < t.fieldEnd; f++)
            {
                if (!Has(Token(Table::Field, f), "KeywordAttribute") || !IsDynamicKeyword(f))
                    continue;
                StringView name = a->Field(f).name;
                for (const StructField& existing : block.fields)
                    if (StringView(existing.name) == name)
                        return Fail(mt.name, Format("two dynamic keywords are named {}", name)), -1;
                StructField& member = block.fields.Emplace();
                member.name = name;
                member.type = kUInt;
            }
        }
        StringView name = a->Field(fieldRow).name;
        for (usize i = 0; i < block.fields.Size(); i++)
            if (StringView(block.fields[i].name) == name)
                return i32(i);
        return Fail(mt.name, Format("dynamic keyword {} is missing from the keyword block", name)), -1;
    }

    bool Translator::StaticField(Method& mt, Token token, Item& out)
    {
        if (token.Is(Table::MemberRef))
        {
            // A resource or keyword declared in an included assembly.
            MemberRefInfo ref = a->MemberRef(token.Row());
            u32 assembly = current, typeRow = 0, fieldRow = 0;
            if (ResolveType(ref.parent, assembly, typeRow))
                fieldRow = assemblies[assembly]->FindField(typeRow, ref.name);
            if (!fieldRow)
                return Fail(mt.name, Format("static field {} of an assembly that is not among the references", ref.name));
            return InAssembly(assembly, [&] { return StaticField(mt, Token(Table::Field, fieldRow), out); });
        }
        if (!token.Is(Table::Field))
            return Fail(mt.name, "unsupported static field reference");
        u32 row = token.Row();
        Token parent(Table::Field, row);
        FieldInfo field = a->Field(row);
        if (Has(parent, "KeywordAttribute"))
        {
            TypeSig sig;
            bool isBool = ParseFieldSignature(field.signature, sig) && sig.kind == ElementType::Boolean;
            if (!isBool && !(sig.kind == ElementType::ValueType && sig.token.Is(Table::TypeDef) && IsEnum(sig.token.Row())))
                return Fail(mt.name, Format("keyword {} must be a bool or an enum declared in the shader assembly", field.name));
            if (IsDynamicKeyword(row))
            {
                // Read at run time from the keyword block.
                i32 member = KeywordMember(mt, row);
                if (member < 0)
                    return false;
                bool known = false;
                for (const KeywordUse& use : m.keywords)
                    known = known || use.name == field.name;
                if (!known)
                {
                    KeywordUse use;
                    use.name = field.name;
                    use.isEnum = !isBool;
                    use.dynamic = true;
                    m.keywords.Push(Move(use));
                }
                const Global& block = m.globals[usize(keywordGlobal)];
                Expr e;
                e.kind = ExprKind::Field;
                e.a = Leaf(mt, ExprKind::Global, u32(keywordGlobal), block.type);
                e.index = u32(member);
                e.type = kUInt;
                ExprId value = AddExpr(mt.fn, Move(e));
                out = {isBool ? MakeBinary(mt.fn, Op::Ne, value, ConstInt(mt, 0, kUInt), kBool) : Cast(mt, value, kInt), false};
                return true;
            }
            // Other keywords are constants of the variant.
            i64 value = 0;
            for (const KeywordValue& k : options.keywords)
                if (k.name == field.name)
                    value = isBool ? i64(k.value != 0) : k.value;
            bool known = false;
            for (const KeywordUse& use : m.keywords)
                known = known || use.name == field.name;
            if (!known)
            {
                KeywordUse use;
                use.name = field.name;
                use.isEnum = !isBool;
                use.value = value;
                m.keywords.Push(Move(use));
            }
            out = {ConstInt(mt, value, isBool ? kBool : kInt), false};
            return true;
        }
        for (const FieldGlobal& g : fieldGlobals)
            if (g.fieldRow == Key(row))
            {
                out = {Leaf(mt, ExprKind::Global, u32(g.global), m.globals[usize(g.global)].type), true};
                return true;
            }
        static constexpr struct
        {
            StringView attribute;
            GlobalKind kind;
        } kKinds[] = {{"UniformAttribute", GlobalKind::Uniform}, {"TextureAttribute", GlobalKind::Texture}, {"SamplerAttribute", GlobalKind::Sampler},
                      {"StorageAttribute", GlobalKind::Storage}};
        Array<AttributeArgument> fixed;
        for (const auto& k : kKinds)
        {
            if (!Has(parent, k.attribute, &fixed) || fixed.Empty())
                continue;
            Global g;
            g.name = field.name;
            g.kind = k.kind;
            g.slot = u32(fixed[0].integer);
            TypeSig sig;
            bool byRef = false;
            if (!ParseFieldSignature(field.signature, sig) || !MapType(sig, &mt, g.type, byRef, mt.name))
                return Fail(mt.name, Format("resource {} has a type that shaders cannot use", field.name));
            if (g.type.IsTexture())
            {
                // The texel type: float, int or uint, one to four components.
                if (sig.kind != ElementType::GenericInst || !MapType(sig.args[0], &mt, g.element, byRef, mt.name) || !g.element.IsNumeric() ||
                    g.element.base == BaseType::Bool || g.element.IsMatrix())
                    return Fail(mt.name, Format("texture {} must have float, int or uint texels", field.name));
            }
            bool image = g.type.base == BaseType::Image;
            if ((k.kind == GlobalKind::Storage) != (g.type.base == BaseType::Buffer || image))
                return Fail(mt.name, Format("{}: [Storage] goes with StructuredBuffer, RWStructuredBuffer and RWTexture2D fields", field.name));
            if (image)
            {
                g.writable = true;
                if (!MapType(sig.args[0], &mt, g.element, byRef, mt.name))
                    return Fail(mt.name, Format("storage texture {} has a texel type that shaders cannot use", field.name));
                // The format is a named argument of the attribute.
                Array<CustomAttribute> attributes;
                a->Attributes(parent, attributes);
                Array<AttributeArgument> f, named;
                for (const CustomAttribute& c : attributes)
                    if (c.typeNamespace == kLibrary && c.typeName == k.attribute && DecodeAttribute(*a, c, f, named))
                        for (const AttributeArgument& n : named)
                            if (n.name == "Format")
                                g.format = StorageFormat(n.integer);
                // The format and the texel type must fit together; Auto follows the texel type.
                Type float4 = Type::Vector(BaseType::Float, 4);
                if (g.format == StorageFormat::Auto)
                    g.format = g.element == kFloat ? StorageFormat::R32F : g.element == kUInt ? StorageFormat::R32UI : g.element == kInt ? StorageFormat::R32I
                                                                                                                                      : StorageFormat::RGBA8;
                bool fits = g.format == StorageFormat::R32F    ? g.element == kFloat
                            : g.format == StorageFormat::R32UI ? g.element == kUInt
                            : g.format == StorageFormat::R32I  ? g.element == kInt
                                                               : g.element == float4;
                if (!fits)
                    return Fail(mt.name, Format("storage texture {}: the texel type does not fit its format (float4 for RGBA formats, float for R32F, "
                                                "uint for R32UI, int for R32I)",
                                                field.name));
            }
            else if (k.kind == GlobalKind::Storage && (!IsBufferType(sig, &g.writable) || !MapType(sig.args[0], &mt, g.element, byRef, mt.name) ||
                                                  !(g.element.IsNumeric() || g.element.base == BaseType::Struct)))
                return Fail(mt.name, Format("buffer {} has an element type that shaders cannot use", field.name));
            m.globals.Push(Move(g));
            fieldGlobals.Push({Key(row), i32(m.globals.Size() - 1)});
            out = {Leaf(mt, ExprKind::Global, u32(m.globals.Size() - 1), m.globals.Back().type), true};
            return true;
        }
        if (field.name.StartsWith("<") && field.name.EndsWith(">k__BackingField"))
            return Fail(mt.name, Format("static property {} keeps its value in a static field, which shaders do not have; use a computed "
                                        "property (=> value) or a constant",
                                        field.name.Substr(1, field.name.Size() - 17)));
        return Fail(mt.name, Format("static field {} is not a resource: mark it [Uniform], [Texture], [Sampler], [Storage] or [Keyword]", field.name));
    }
}
