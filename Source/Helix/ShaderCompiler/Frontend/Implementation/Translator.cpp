#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/ShaderCompiler/Transforms/Simplify.h>
#include <Helix/ShaderCompiler/Frontend/Implementation/Flow.h>

#include <Helix/Core/Strings/Format.h>
#include <Helix/ShaderCompiler/Assembly/IL.h>
#include <Helix/ShaderCompiler/Assembly/Signature.h>

namespace helix::shadercompiler
{
    namespace
    {
        constexpr StringView kLibrary = "HelixEngine.Shaders";

        // A value on the IL evaluation stack. Addresses (ldloca, ldflda, ref parameters) keep the expression of the
        // place they point to; reading through one yields the same expression.
        struct Item
        {
            ExprId e = 0;
            bool address = false;
        };

        // Vector and matrix types of the shader library by name: float3, int2, bool4, float4x4, ...
        bool LibraryType(StringView name, Type& out)
        {
            static constexpr struct
            {
                StringView prefix;
                BaseType base;
            } kBases[] = {{"float", BaseType::Float}, {"int", BaseType::Int}, {"uint", BaseType::UInt}, {"bool", BaseType::Bool}};
            // A bindless resource is the index it was registered under.
            if (name == "BindlessTexture" || name == "BindlessBuffer")
            {
                out = kUInt;
                return true;
            }
            if (name == "RWTexture2D")
            {
                out = Type::Scalar(BaseType::Image);
                return true;
            }
            if (name == "SamplerState")
            {
                out = Type::Scalar(BaseType::Sampler);
                return true;
            }
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

        i32 ComponentIndex(char c) { return c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : c == 'w' ? 3 : -1; }

        struct Translator
        {
            const Assembly& a;
            Module& m;
            String& error;

            const TranslateOptions& options;

            Translator(const Assembly& assembly, Module& module, String& e, const TranslateOptions& o) : a(assembly), m(module), error(e), options(o) {}

            Array<u32> structRows;
            struct FieldGlobal
            {
                u32 fieldRow;
                i32 global;
            };
            Array<FieldGlobal> fieldGlobals;
            struct Compiled
            {
                String key;
                i32 index;
            };
            Array<Compiled> compiled;
            Array<String> building;

            // ---- per method state ----
            struct Method
            {
                Function fn;
                StringView name;
                MethodSig sig;
                Array<Type> typeArgs;
                Array<u8> temporary; // per local: created by the translation, written once before use
                // Per IL local: a ref local (the C# compiler makes them for compound assignments) and the place it refers to.
                Array<u8> isRef;
                Array<ExprId> refTarget;
                Array<Instruction> code;
                Array<i32> blockOfOffset;
                Array<FlowBlock> blocks;
                struct BlockInfo
                {
                    u32 first = 0, last = 0;
                    Array<Item> entry;
                    bool hasEntry = false;
                    bool done = false;
                };
                Array<BlockInfo> info;
                Array<Item> stack;
                Array<Stmt>* stmts = nullptr;
            };

            bool Fail(StringView method, StringView message)
            {
                if (error.Empty())
                    error = Format("{}: {}", method, message);
                return false;
            }

            // ---------------------------------------------------------------- types

            bool Has(Token parent, StringView attribute, Array<AttributeArgument>* fixed = nullptr) const
            {
                Array<CustomAttribute> attributes;
                a.Attributes(parent, attributes);
                Array<AttributeArgument> f, n;
                for (const CustomAttribute& c : attributes)
                    if (c.typeNamespace == kLibrary && c.typeName == attribute)
                    {
                        if (fixed && !DecodeAttribute(a, c, *fixed, n))
                            return false;
                        return true;
                    }
                return false;
            }

            Semantic ReadSemantic(Token parent, u32& index, bool& flat) const
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

            i32 StructFor(u32 typeRow, StringView method)
            {
                for (usize i = 0; i < structRows.Size(); i++)
                    if (structRows[i] == typeRow)
                        return i32(i);
                TypeDefInfo t = a.TypeDef(typeRow);
                StructType s;
                s.name = t.name;
                for (u32 f = t.fieldBegin; f < t.fieldEnd; f++)
                {
                    FieldInfo field = a.Field(f);
                    if (field.IsStatic())
                        continue;
                    TypeSig sig;
                    StructField& out = s.fields.Emplace();
                    bool byRef = false;
                    if (!ParseFieldSignature(field.signature, sig) || !MapType(sig, nullptr, out.type, byRef, method))
                    {
                        Fail(method, Format("field {}.{} has a type that shaders cannot use", t.name, field.name));
                        return -1;
                    }
                    out.name = field.name;
                    out.semantic = ReadSemantic(Token(Table::Field, f), out.semanticIndex, out.flat);
                }
                // Register after the fields, so nested structs come first.
                structRows.Push(typeRow);
                m.structs.Push(Move(s));
                return i32(m.structs.Size() - 1);
            }

            // Enums are their integer value.
            bool IsEnum(u32 typeRow) const
            {
                StringView ns, name;
                return a.TypeName(a.TypeDef(typeRow).extends, ns, name) && ns == "System" && name == "Enum";
            }

            bool MapType(const TypeSig& sig, const Method* method, Type& out, bool& byRef, StringView where)
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
                    if (!a.TypeName(sig.token, ns, name))
                        return false;
                    if (ns == kLibrary)
                        return LibraryType(name, out);
                    if (!sig.token.Is(Table::TypeDef))
                        return false;
                    if (IsEnum(sig.token.Row()))
                    {
                        out = kInt;
                        return true;
                    }
                    i32 index = StructFor(sig.token.Row(), where);
                    if (index < 0)
                        return false;
                    out = Type::Struct(u32(index));
                    return true;
                }
                case ElementType::GenericInst:
                    if (IsTextureType(sig))
                    {
                        out = Type::Scalar(BaseType::Texture2D);
                        return true;
                    }
                    if (!IsBufferType(sig))
                        return false;
                    out = Type::Scalar(BaseType::Buffer);
                    return true;
                default: return false;
                }
            }

            // Texture2D<T> of the shader library.
            bool IsTextureType(const TypeSig& sig) const
            {
                StringView ns, name;
                return sig.kind == ElementType::GenericInst && sig.args.Size() == 1 && a.TypeName(sig.token, ns, name) && ns == kLibrary &&
                       name.StartsWith("Texture2D");
            }

            // StructuredBuffer<T> or RWStructuredBuffer<T> of the shader library.
            bool IsBufferType(const TypeSig& sig, bool* writable = nullptr) const
            {
                StringView ns, name;
                if (sig.kind != ElementType::GenericInst || sig.args.Size() != 1 || !a.TypeName(sig.token, ns, name) || ns != kLibrary)
                    return false;
                if (writable)
                    *writable = name.StartsWith("RW");
                return name.StartsWith("StructuredBuffer") || name.StartsWith("RWStructuredBuffer");
            }

            // ---------------------------------------------------------------- expressions

            static Type TypeOf(const Method& mt, ExprId e) { return mt.fn.exprs[e].type; }

            static ExprId Leaf(Method& mt, ExprKind kind, u32 index, Type type)
            {
                Expr e;
                e.kind = kind;
                e.index = index;
                e.type = type;
                return AddExpr(mt.fn, Move(e));
            }

            static ExprId ConstInt(Method& mt, i64 value, Type type)
            {
                Expr e;
                e.kind = type.base == BaseType::Bool ? ExprKind::ConstBool : ExprKind::ConstInt;
                e.integer = type.base == BaseType::Bool ? (value != 0) : type.base == BaseType::UInt ? i64(u32(value)) : value;
                e.type = type;
                return AddExpr(mt.fn, Move(e));
            }

            static ExprId ConstFloat(Method& mt, f64 value)
            {
                Expr e;
                e.kind = ExprKind::ConstFloat;
                e.number = value;
                e.type = kFloat;
                return AddExpr(mt.fn, Move(e));
            }

            static ExprId Cast(Method& mt, ExprId value, Type to)
            {
                Expr e;
                e.kind = ExprKind::Cast;
                e.a = value;
                e.type = to;
                return AddExpr(mt.fn, Move(e));
            }

            // IL has one integer type on the stack; the IR keeps int, uint and bool apart and converts at the uses.
            static ExprId Coerce(Method& mt, ExprId value, Type to)
            {
                Expr e = mt.fn.exprs[value];
                Type from = e.type;
                if (from == to || !to.IsScalar() || !from.IsScalar())
                    return value;
                if ((e.kind == ExprKind::ConstInt || e.kind == ExprKind::ConstBool) && to.base != BaseType::Float)
                    return ConstInt(mt, e.integer, to);
                if (to.base == BaseType::Bool)
                    return MakeBinary(mt.fn, Op::Ne, value, from.base == BaseType::Float ? ConstFloat(mt, 0.0) : ConstInt(mt, 0, from), kBool);
                if (from.base == BaseType::Float && to.base == BaseType::Float)
                    return value;
                return Cast(mt, value, to);
            }

            static u32 NewLocal(Method& mt, Type type, bool temporary)
            {
                Variable& v = mt.fn.locals.Emplace();
                v.type = type;
                v.name = Format(temporary ? "t{}" : "l{}", mt.fn.locals.Size() - 1);
                mt.temporary.Push(temporary ? 1 : 0);
                return u32(mt.fn.locals.Size() - 1);
            }

            // True when the value depends on memory a later statement could change.
            static bool ReadsState(const Method& mt, ExprId id)
            {
                const Expr& e = mt.fn.exprs[id];
                switch (e.kind)
                {
                case ExprKind::ConstFloat: case ExprKind::ConstInt: case ExprKind::ConstBool: return false;
                case ExprKind::Local: return !mt.temporary[e.index];
                case ExprKind::Param: case ExprKind::Global: case ExprKind::Call: return true;
                case ExprKind::Intrinsic:
                    if (e.intrinsic == Intrinsic::BindlessLoad)
                        return true;
                    break;
                default: break;
                }
                if ((e.a && ReadsState(mt, e.a)) || (e.b && ReadsState(mt, e.b)))
                    return true;
                for (ExprId arg : e.args)
                    if (ReadsState(mt, arg))
                        return true;
                return false;
            }

            static void Spill(Method& mt, Item& item)
            {
                u32 local = NewLocal(mt, TypeOf(mt, item.e), true);
                Stmt& s = mt.stmts->Emplace();
                s.kind = StmtKind::Assign;
                s.a = Leaf(mt, ExprKind::Local, local, TypeOf(mt, item.e));
                s.b = item.e;
                item.e = s.a;
            }

            // Values still waiting on the stack must keep what they read before a statement changes memory.
            static void Flush(Method& mt)
            {
                for (Item& item : mt.stack)
                    if (!item.address && ReadsState(mt, item.e))
                        Spill(mt, item);
            }

            static void Assign(Method& mt, ExprId target, ExprId value)
            {
                Flush(mt);
                Stmt& s = mt.stmts->Emplace();
                s.kind = StmtKind::Assign;
                s.a = target;
                s.b = Coerce(mt, value, TypeOf(mt, target));
            }

            static Item Pop(Method& mt)
            {
                if (mt.stack.Empty())
                    return {};
                Item item = mt.stack.Back();
                mt.stack.Pop();
                return item;
            }

            static void Push(Method& mt, ExprId e, bool address = false) { mt.stack.Push({e, address}); }

            static ExprId Swizzle(Method& mt, ExprId value, const u8* components, u32 count)
            {
                Type t = TypeOf(mt, value);
                Expr e;
                e.kind = ExprKind::Swizzle;
                e.a = value;
                e.swizzleCount = u8(count);
                for (u32 i = 0; i < count; i++)
                    e.swizzle[i] = components[i];
                e.type = count == 1 ? t.ScalarType() : Type::Vector(t.base, count);
                return AddExpr(mt.fn, Move(e));
            }

            static ExprId Index(Method& mt, ExprId value, ExprId index)
            {
                Type t = TypeOf(mt, value);
                Expr e;
                e.kind = ExprKind::Index;
                e.a = value;
                e.b = Coerce(mt, index, kInt);
                e.type = t.IsMatrix() ? Type::Vector(BaseType::Float, t.rows) : t.ScalarType();
                return AddExpr(mt.fn, Move(e));
            }

            // Brings the operands of an arithmetic or comparison instruction to one type.
            static void Unify(Method& mt, ExprId& x, ExprId& y, bool asUnsigned)
            {
                Type tx = TypeOf(mt, x), ty = TypeOf(mt, y);
                bool cx = mt.fn.exprs[x].kind == ExprKind::ConstInt, cy = mt.fn.exprs[y].kind == ExprKind::ConstInt;
                if (!(tx == ty) && tx.IsScalar() && ty.IsScalar())
                {
                    if (cx && !cy)
                        x = Coerce(mt, x, ty);
                    else if (cy)
                        y = Coerce(mt, y, tx);
                    else if (tx.base == BaseType::Bool)
                        x = Coerce(mt, x, ty);
                    else if (ty.base == BaseType::Bool)
                        y = Coerce(mt, y, tx);
                    else if (tx.IsInteger() && ty.IsInteger())
                    {
                        x = Coerce(mt, x, kUInt);
                        y = Coerce(mt, y, kUInt);
                    }
                }
                if (asUnsigned && TypeOf(mt, x).base == BaseType::Int)
                {
                    x = Coerce(mt, x, kUInt);
                    y = Coerce(mt, y, kUInt);
                }
            }

            static ExprId Arithmetic(Method& mt, Op op, ExprId x, ExprId y, bool asUnsigned)
            {
                if (op == Op::Shl || op == Op::Shr)
                {
                    if (asUnsigned && TypeOf(mt, x).base == BaseType::Int)
                        x = Coerce(mt, x, kUInt);
                    return MakeBinary(mt.fn, op, x, Coerce(mt, y, kInt), TypeOf(mt, x));
                }
                Unify(mt, x, y, asUnsigned);
                Type t = TypeOf(mt, x);
                if (t.base == BaseType::Bool && (op == Op::And || op == Op::Or || op == Op::Xor))
                    return MakeBinary(mt.fn, op == Op::And ? Op::LogicalAnd : op == Op::Or ? Op::LogicalOr : Op::Ne, x, y, kBool);
                return MakeBinary(mt.fn, op, x, y, t);
            }

            // `unordered` is the .un form: unsigned for integers, true for NaN operands for floats.
            static ExprId Compare(Method& mt, Op op, ExprId x, ExprId y, bool unordered)
            {
                const Expr ex = mt.fn.exprs[x], ey = mt.fn.exprs[y];
                // Tests of a bool against 0 or 1 are the bool itself or its negation.
                if (ex.type.base == BaseType::Bool && ey.kind == ExprKind::ConstInt && ex.type.IsScalar())
                {
                    bool same = (op == Op::Eq) == (ey.integer != 0) || (op == Op::Gt && ey.integer == 0) || (op == Op::Ne && ey.integer == 0);
                    if (op == Op::Eq || op == Op::Ne || op == Op::Gt)
                        return same ? x : MakeNot(mt.fn, x);
                }
                // cgt.un against zero is the "not zero" test.
                if (unordered && op == Op::Gt && ey.kind == ExprKind::ConstInt && ey.integer == 0 && ex.type.IsInteger())
                    return MakeBinary(mt.fn, Op::Ne, x, Coerce(mt, y, ex.type), kBool);
                bool isFloat = ex.type.base == BaseType::Float;
                Unify(mt, x, y, unordered && !isFloat);
                if (unordered && isFloat && op != Op::Ne && op != Op::Eq)
                {
                    static constexpr Op kOpposite[] = {Op::Ge, Op::Gt, Op::Le, Op::Lt};
                    return MakeUnary(mt.fn, Op::Not, MakeBinary(mt.fn, kOpposite[u32(op) - u32(Op::Lt)], x, y, kBool), kBool);
                }
                return MakeBinary(mt.fn, op, x, y, kBool);
            }

            // ---------------------------------------------------------------- members

            // Field of a struct or component of a vector or matrix, for ldfld, ldflda and stfld.
            bool FieldExpr(Method& mt, Token token, ExprId object, ExprId& out)
            {
                StringView name, ns, typeName;
                if (token.Is(Table::Field))
                {
                    u32 typeRow = a.DeclaringTypeOfField(token.Row());
                    i32 index = StructFor(typeRow, mt.name);
                    if (index < 0)
                        return false;
                    name = a.Field(token.Row()).name;
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
                if (!token.Is(Table::MemberRef))
                    return Fail(mt.name, "unsupported field reference");
                MemberRefInfo ref = a.MemberRef(token.Row());
                Type type;
                if (!a.TypeName(ref.parent, ns, typeName) || ns != kLibrary || !LibraryType(typeName, type))
                    return Fail(mt.name, Format("field {} of a type that shaders cannot use", ref.name));
                if (type.IsMatrix() && ref.name.Size() == 2 && ref.name[0] == 'c')
                {
                    out = Index(mt, object, ConstInt(mt, ref.name[1] - '0', kInt));
                    return true;
                }
                i32 component = ref.name.Size() == 1 ? ComponentIndex(ref.name[0]) : -1;
                if (!type.IsVector() || component < 0)
                    return Fail(mt.name, Format("unknown field {}.{}", typeName, ref.name));
                u8 c = u8(component);
                out = Swizzle(mt, object, &c, 1);
                return true;
            }

            // Static fields are resources (uniform block, texture, sampler) or keywords.
            bool StaticField(Method& mt, Token token, Item& out)
            {
                if (!token.Is(Table::Field))
                    return Fail(mt.name, "static fields of other assemblies are not available in shaders");
                u32 row = token.Row();
                Token parent(Table::Field, row);
                FieldInfo field = a.Field(row);
                if (Has(parent, "KeywordAttribute"))
                {
                    // Keywords are constants of the variant.
                    TypeSig sig;
                    bool isBool = ParseFieldSignature(field.signature, sig) && sig.kind == ElementType::Boolean;
                    if (!isBool && !(sig.kind == ElementType::ValueType && sig.token.Is(Table::TypeDef) && IsEnum(sig.token.Row())))
                        return Fail(mt.name, Format("keyword {} must be a bool or an enum declared in the shader assembly", field.name));
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
                    if (g.fieldRow == row)
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
                    if (g.type.base == BaseType::Texture2D)
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
                        g.element = Type::Vector(BaseType::Float, 4);
                        // The format is a named argument of the attribute.
                        Array<CustomAttribute> attributes;
                        a.Attributes(parent, attributes);
                        Array<AttributeArgument> f, named;
                        for (const CustomAttribute& c : attributes)
                            if (c.typeNamespace == kLibrary && c.typeName == k.attribute && DecodeAttribute(a, c, f, named))
                                for (const AttributeArgument& n : named)
                                    if (n.name == "Format")
                                        g.format = StorageFormat(n.integer);
                    }
                    else if (k.kind == GlobalKind::Storage && (!IsBufferType(sig, &g.writable) || !MapType(sig.args[0], &mt, g.element, byRef, mt.name) ||
                                                          !(g.element.IsNumeric() || g.element.base == BaseType::Struct)))
                        return Fail(mt.name, Format("buffer {} has an element type that shaders cannot use", field.name));
                    m.globals.Push(Move(g));
                    fieldGlobals.Push({row, i32(m.globals.Size() - 1)});
                    out = {Leaf(mt, ExprKind::Global, u32(m.globals.Size() - 1), m.globals.Back().type), true};
                    return true;
                }
                return Fail(mt.name, Format("static field {} is not a resource: mark it [Uniform], [Texture], [Sampler], [Storage] or [Keyword]", field.name));
            }

            // ---------------------------------------------------------------- calls

            // Members of Texture2D<T>: results have the texel type of the texture. The sampler travels with the texture slot,
            // so only coordinates and level remain of the arguments.
            bool TextureCall(Method& mt, StringView member, Array<Item>& args)
            {
                const Expr& self = mt.fn.exprs[args[0].e];
                if (self.kind != ExprKind::Global || m.globals[self.index].type.base != BaseType::Texture2D)
                    return Fail(mt.name, "a texture must be used directly from its static field");
                Type element = m.globals[self.index].element;
                Expr e;
                e.kind = ExprKind::Intrinsic;
                e.type = element;
                e.args.Push(args[0].e);
                if (member == "Sample" || member == "SampleLevel")
                {
                    if (element.base != BaseType::Float)
                        return Fail(mt.name, Format("{} has integer texels and cannot be sampled; use Load", m.globals[self.index].name));
                    e.intrinsic = member == "Sample" ? Intrinsic::TextureSample : Intrinsic::TextureSampleLevel;
                    e.args.Push(Coerce(mt, args[2].e, Type::Vector(BaseType::Float, 2)));
                    if (args.Size() > 3)
                        e.args.Push(Coerce(mt, args[3].e, kFloat));
                }
                else if (member == "Load" && args.Size() == 3)
                {
                    e.intrinsic = Intrinsic::TextureLoad;
                    e.args.Push(Coerce(mt, args[1].e, Type::Vector(BaseType::Int, 2)));
                    e.args.Push(Coerce(mt, args[2].e, kInt));
                }
                else if (member == "get_Size")
                {
                    e.intrinsic = Intrinsic::TextureSize;
                    e.type = Type::Vector(BaseType::Int, 2);
                }
                else
                    return Fail(mt.name, Format("Texture2D.{} is not available in shaders", member));
                Push(mt, AddExpr(mt.fn, Move(e)));
                return true;
            }

            // Indexer and length of a storage buffer; an element is the Index expression of the buffer.
            bool BufferCall(Method& mt, StringView member, Array<Item>& args)
            {
                const Expr& self = mt.fn.exprs[args[0].e];
                if (self.kind != ExprKind::Global || m.globals[self.index].kind != GlobalKind::Storage)
                    return Fail(mt.name, "a buffer must be used directly from its static field");
                Type element = m.globals[self.index].element;
                bool writable = m.globals[self.index].writable;
                StringView name = m.globals[self.index].name;
                Expr e;
                if (member == "get_Length")
                {
                    e.kind = ExprKind::Intrinsic;
                    e.intrinsic = Intrinsic::BufferLength;
                    e.type = kInt;
                    e.args.Push(args[0].e);
                    Push(mt, AddExpr(mt.fn, Move(e)));
                    return true;
                }
                if ((member != "get_Item" && member != "set_Item") || args.Size() < 2)
                    return Fail(mt.name, Format("buffer member {} is not available in shaders", member));
                e.kind = ExprKind::Index;
                e.a = args[0].e;
                e.b = Coerce(mt, args[1].e, kInt);
                e.type = element;
                ExprId item = AddExpr(mt.fn, Move(e));
                if (member == "get_Item")
                    Push(mt, item);
                else if (!writable || args.Size() != 3)
                    return Fail(mt.name, Format("buffer {} is read only", name));
                else
                    Assign(mt, item, args[2].e);
                return true;
            }

            // Members of the vector and matrix types and the functions of the shader library.
            bool LibraryCall(Method& mt, StringView typeName, StringView member, const MethodSig& sig, Array<Item>& args, bool isNew)
            {
                Type result = kVoid, self;
                bool byRef = false;
                if (!MapType(sig.returnType, &mt, result, byRef, mt.name))
                    return Fail(mt.name, Format("{}.{} returns a type that shaders cannot use", typeName, member));
                // Arguments take the parameter types, which settles int against uint and bool.
                u32 first = sig.hasThis && !isNew ? 1 : 0;
                for (usize i = 0; i < sig.params.Size() && first + i < args.Size(); i++)
                {
                    Type p;
                    if (MapType(sig.params[i], &mt, p, byRef, mt.name))
                        args[first + i].e = Coerce(mt, args[first + i].e, p);
                }
                auto make = [&](Expr&& e) {
                    e.type = result;
                    for (usize i = first; i < args.Size(); i++)
                        e.args.Push(args[i].e);
                    return AddExpr(mt.fn, Move(e));
                };

                if (typeName == "ShaderMath")
                {
                    if (member == "Discard")
                    {
                        Flush(mt);
                        mt.stmts->Emplace().kind = StmtKind::Discard;
                        return true;
                    }
                    char lower[64] = {};
                    for (usize i = 0; i < member.Size() && i < 63; i++)
                        lower[i] = member[i] >= 'A' && member[i] <= 'Z' ? char(member[i] + 32) : member[i];
                    Expr e;
                    e.kind = ExprKind::Intrinsic;
                    e.intrinsic = FindIntrinsic(lower);
                    if (e.intrinsic == Intrinsic::Count)
                        return Fail(mt.name, Format("ShaderMath.{} is not known to the compiler", member));
                    if (e.intrinsic == Intrinsic::InterlockedAdd)
                    {
                        const Expr& buffer = mt.fn.exprs[args[0].e];
                        if (buffer.kind != ExprKind::Global || m.globals[buffer.index].kind != GlobalKind::Storage)
                            return Fail(mt.name, "a buffer must be used directly from its static field");
                        // It changes the buffer, so it runs exactly here and its result waits in a temporary.
                        Flush(mt);
                        Item previous{make(Move(e)), false};
                        Spill(mt, previous);
                        Push(mt, previous.e);
                        return true;
                    }
                    Push(mt, make(Move(e)));
                    return true;
                }
                if (typeName == "BindlessTexture" || typeName == "BindlessBuffer")
                {
                    if (member == ".ctor")
                    {
                        if (isNew)
                            Push(mt, Coerce(mt, args[0].e, kUInt));
                        else
                            Assign(mt, args[0].e, args[1].e);
                        return true;
                    }
                    if (member == "get_Index")
                    {
                        Push(mt, args[0].e);
                        return true;
                    }
                    static constexpr struct
                    {
                        StringView member;
                        Intrinsic intrinsic;
                    } kMembers[] = {{"Sample", Intrinsic::BindlessSample}, {"SampleLevel", Intrinsic::BindlessSampleLevel},
                                    {"get_Size", Intrinsic::BindlessSize},  {"Load", Intrinsic::BindlessLoad},
                                    {"Load2", Intrinsic::BindlessLoad},     {"Load3", Intrinsic::BindlessLoad},
                                    {"Load4", Intrinsic::BindlessLoad},     {"Store", Intrinsic::BindlessStore},
                                    {"Store2", Intrinsic::BindlessStore},   {"Store3", Intrinsic::BindlessStore},
                                    {"Store4", Intrinsic::BindlessStore}};
                    for (const auto& k : kMembers)
                    {
                        if (member != k.member)
                            continue;
                        Expr e;
                        e.kind = ExprKind::Intrinsic;
                        e.intrinsic = k.intrinsic;
                        e.type = result;
                        e.args.Push(args[0].e);
                        for (usize i = 1; i < args.Size(); i++)
                            e.args.Push(args[i].e);
                        ExprId value = AddExpr(mt.fn, Move(e));
                        if (k.intrinsic == Intrinsic::BindlessStore)
                        {
                            // A write to memory runs exactly here.
                            Flush(mt);
                            Stmt& s = mt.stmts->Emplace();
                            s.kind = StmtKind::Evaluate;
                            s.a = value;
                        }
                        else
                            Push(mt, value);
                        return true;
                    }
                    return Fail(mt.name, Format("{}.{} is not available in shaders", typeName, member));
                }
                if (typeName == "RWTexture2D")
                {
                    const Expr& image = mt.fn.exprs[args[0].e];
                    if (image.kind != ExprKind::Global || m.globals[image.index].type.base != BaseType::Image)
                        return Fail(mt.name, "a storage texture must be used directly from its static field");
                    if (member != "set_Item" || args.Size() != 3)
                        return Fail(mt.name, Format("RWTexture2D.{} is not available in shaders; a storage texture can only be written", member));
                    Expr e;
                    e.kind = ExprKind::Index;
                    e.a = args[0].e;
                    e.b = Coerce(mt, args[1].e, Type::Vector(BaseType::Int, 2));
                    e.type = Type::Vector(BaseType::Float, 4);
                    Assign(mt, AddExpr(mt.fn, Move(e)), args[2].e);
                    return true;
                }
                if (!LibraryType(typeName, self) || !self.IsNumeric())
                    return Fail(mt.name, Format("{}.{} is not available in shaders", typeName, member));

                if (member == ".ctor")
                {
                    Expr e;
                    e.kind = ExprKind::Construct;
                    for (usize i = first; i < args.Size(); i++)
                        e.args.Push(args[i].e);
                    e.type = self;
                    ExprId value = AddExpr(mt.fn, Move(e));
                    if (isNew)
                        Push(mt, value);
                    else
                        Assign(mt, args[0].e, value);
                    return true;
                }
                static constexpr struct
                {
                    StringView name;
                    Op op;
                    u8 operands;
                } kOperators[] = {{"op_Addition", Op::Add, 2},         {"op_Subtraction", Op::Sub, 2},  {"op_Multiply", Op::Mul, 2},
                                  {"op_Division", Op::Div, 2},         {"op_Modulus", Op::Rem, 2},      {"op_UnaryNegation", Op::Neg, 1},
                                  {"op_LessThan", Op::Lt, 2},          {"op_GreaterThan", Op::Gt, 2},   {"op_LessThanOrEqual", Op::Le, 2},
                                  {"op_GreaterThanOrEqual", Op::Ge, 2}, {"op_Equality", Op::Eq, 2},      {"op_Inequality", Op::Ne, 2},
                                  {"op_BitwiseAnd", Op::And, 2},       {"op_BitwiseOr", Op::Or, 2},     {"op_ExclusiveOr", Op::Xor, 2},
                                  {"op_LeftShift", Op::Shl, 2},        {"op_RightShift", Op::Shr, 2},   {"op_LogicalNot", Op::Not, 1}};
                for (const auto& o : kOperators)
                    if (member == o.name && args.Size() == o.operands)
                    {
                        Push(mt, o.operands == 1 ? MakeUnary(mt.fn, o.op, args[0].e, result) : MakeBinary(mt.fn, o.op, args[0].e, args[1].e, result));
                        return true;
                    }
                if (member == "op_Explicit" && args.Size() == 1)
                {
                    Push(mt, Cast(mt, args[0].e, result));
                    return true;
                }
                if (member == "get_Identity" && self.IsMatrix())
                {
                    Expr e;
                    e.kind = ExprKind::Construct;
                    e.type = self;
                    e.args.Push(ConstFloat(mt, 1.0));
                    Push(mt, AddExpr(mt.fn, Move(e)));
                    return true;
                }
                if (member == "get_Item" && args.Size() == 2)
                {
                    Push(mt, Index(mt, args[0].e, args[1].e));
                    return true;
                }
                if (member == "set_Item" && args.Size() == 3)
                {
                    Assign(mt, Index(mt, args[0].e, args[1].e), args[2].e);
                    return true;
                }
                bool getter = member.StartsWith("get_"), setter = member.StartsWith("set_");
                if ((getter || setter) && self.IsVector())
                {
                    StringView names = member.Substr(4);
                    u8 components[4];
                    bool valid = names.Size() >= 1 && names.Size() <= 4;
                    for (usize i = 0; valid && i < names.Size(); i++)
                    {
                        i32 c = ComponentIndex(names[i]);
                        valid = c >= 0 && u32(c) < self.rows;
                        components[i] = u8(c);
                    }
                    if (valid)
                    {
                        ExprId swizzle = Swizzle(mt, args[0].e, components, u32(names.Size()));
                        if (getter)
                            Push(mt, swizzle);
                        else
                            Assign(mt, swizzle, args[1].e);
                        return true;
                    }
                }
                return Fail(mt.name, Format("{}.{} is not available in shaders", typeName, member));
            }

            i32 FunctionFor(u32 methodRow, const Array<Type>& typeArgs, StringView caller)
            {
                String key = Format("{}", methodRow);
                for (const Type& t : typeArgs)
                    FormatAppend(key, ",{}.{}.{}.{}", u32(t.base), u32(t.rows), u32(t.cols), u32(t.structIndex));
                for (const Compiled& c : compiled)
                    if (c.key == key)
                        return c.index;
                for (const String& b : building)
                    if (b == key)
                    {
                        Fail(caller, Format("{} is recursive, shaders cannot recurse", a.Method(methodRow).name));
                        return -1;
                    }
                building.Push(key);
                Function fn;
                if (!TranslateMethod(methodRow, typeArgs, fn))
                    return -1;
                building.Pop();
                fn.name = Format("f_{}_{}", a.Method(methodRow).name, m.functions.Size());
                m.functions.Push(Move(fn));
                compiled.Push({Move(key), i32(m.functions.Size() - 1)});
                return i32(m.functions.Size() - 1);
            }

            bool Call(Method& mt, Token token, bool isNew)
            {
                Array<Type> typeArgs;
                if (token.Is(Table::MethodSpec))
                {
                    Array<TypeSig> sigs;
                    if (!ParseMethodSpec(a.GetBlob(a.Cell(Table::MethodSpec, token.Row(), 1)), sigs))
                        return Fail(mt.name, "damaged generic method instantiation");
                    for (const TypeSig& s : sigs)
                    {
                        bool byRef = false;
                        if (!MapType(s, &mt, typeArgs.Emplace(), byRef, mt.name))
                            return Fail(mt.name, "a generic method is used with a type that shaders cannot use");
                    }
                    token = a.CodedCell(Table::MethodSpec, token.Row(), 0);
                }
                MethodSig sig;
                StringView ns, typeName, member;
                bool user = token.Is(Table::MethodDef), buffer = false, texture = false;
                if (user)
                {
                    MethodInfo info = a.Method(token.Row());
                    member = info.name;
                    if (!ParseMethodSignature(info.signature, sig))
                        return Fail(mt.name, "damaged method signature");
                }
                else if (token.Is(Table::MemberRef))
                {
                    MemberRefInfo ref = a.MemberRef(token.Row());
                    member = ref.name;
                    TypeSig spec;
                    bool generic = ref.parent.Is(Table::TypeSpec) && ParseTypeSpec(a.TypeSpecSignature(ref.parent.Row()), spec);
                    if (generic && IsTextureType(spec))
                    {
                        texture = true;
                        if (!ParseMethodSignature(ref.signature, sig))
                            return Fail(mt.name, "damaged method signature");
                    }
                    else if (generic && IsBufferType(spec))
                    {
                        buffer = true;
                        if (!ParseMethodSignature(ref.signature, sig))
                            return Fail(mt.name, "damaged method signature");
                    }
                    else if (!ParseMethodSignature(ref.signature, sig) || !a.TypeName(ref.parent, ns, typeName))
                        return Fail(mt.name, Format("call to {} cannot be resolved (generic library types are not supported yet)", ref.name));
                }
                else
                    return Fail(mt.name, "unsupported call target");

                u32 count = u32(sig.params.Size()) + (sig.hasThis && !isNew ? 1 : 0);
                if (mt.stack.Size() < count)
                    return Fail(mt.name, "evaluation stack underflow at a call");
                Array<Item> args;
                args.Resize(count);
                for (u32 i = count; i-- > 0;)
                    args[i] = Pop(mt);

                if (buffer)
                    return BufferCall(mt, member, args);
                if (texture)
                    return TextureCall(mt, member, args);
                if (!user)
                {
                    if (ns != kLibrary)
                        return Fail(mt.name, Format("{}.{}.{} is not available in shaders", ns, typeName, member));
                    return LibraryCall(mt, typeName, member, sig, args, isNew);
                }
                if (isNew)
                    return Fail(mt.name, Format("constructor {} of a user type: use a static method or set the fields", member));
                i32 index = FunctionFor(token.Row(), typeArgs, mt.name);
                if (index < 0)
                    return false;
                const Function& callee = m.functions[usize(index)];
                Expr e;
                e.kind = ExprKind::Call;
                e.index = u32(index);
                e.type = callee.returnType;
                bool writes = false;
                for (usize i = 0; i < args.Size(); i++)
                {
                    ParamMode mode = callee.params[i].mode;
                    writes = writes || mode != ParamMode::In;
                    e.args.Push(mode == ParamMode::In ? Coerce(mt, args[i].e, callee.params[i].type) : args[i].e);
                }
                ExprId call = AddExpr(mt.fn, Move(e));
                if (callee.returnType.IsVoid())
                {
                    Flush(mt);
                    Stmt& s = mt.stmts->Emplace();
                    s.kind = StmtKind::Evaluate;
                    s.a = call;
                }
                else if (writes)
                {
                    // The call changes its arguments, so it runs exactly here.
                    Flush(mt);
                    Item result{call, false};
                    Spill(mt, result);
                    Push(mt, result.e);
                }
                else
                    Push(mt, call);
                return true;
            }

            // ---------------------------------------------------------------- blocks

            // Values left on the stack at the end of a block travel to the next blocks in temporaries.
            void LeaveBlock(Method& mt, i32 target)
            {
                Method::BlockInfo& info = mt.info[usize(target)];
                if (!info.hasEntry)
                {
                    info.hasEntry = true;
                    for (const Item& item : mt.stack)
                    {
                        Type t = TypeOf(mt, item.e);
                        info.entry.Push({item.address ? item.e : Leaf(mt, ExprKind::Local, NewLocal(mt, t, true), t), item.address});
                    }
                }
                for (usize i = 0; i < mt.stack.Size() && i < info.entry.Size(); i++)
                {
                    if (mt.stack[i].address)
                        continue;
                    Stmt& s = mt.stmts->Emplace();
                    s.kind = StmtKind::Assign;
                    s.a = info.entry[i].e;
                    s.b = Coerce(mt, mt.stack[i].e, TypeOf(mt, s.a));
                }
            }

            bool TranslateBlock(Method& mt, u32 blockIndex)
            {
                mt.stack = mt.info[blockIndex].entry;
                mt.stmts = &mt.blocks[blockIndex].stmts;
                u32 first = mt.info[blockIndex].first, last = mt.info[blockIndex].last;
                auto local = [&](u32 i) { return Leaf(mt, ExprKind::Local, i, mt.fn.locals[i].type); };
                auto param = [&](u32 i) { return Leaf(mt, ExprKind::Param, i, mt.fn.params[i].type); };
                auto target = [&](u32 offset) { return offset < mt.blockOfOffset.Size() ? mt.blockOfOffset[offset] : -1; };
                auto finish = [&](FlowBlock::End end, ExprId value, i32 yes, i32 no) {
                    // Blocks may have been appended, so the block is looked up again.
                    mt.stmts = &mt.blocks[blockIndex].stmts;
                    if (yes >= 0)
                        LeaveBlock(mt, yes);
                    if (no >= 0 && !mt.info[usize(no)].hasEntry)
                    {
                        // Both targets receive the same values; the temporaries of the first are reused.
                        mt.info[usize(no)].hasEntry = true;
                        mt.info[usize(no)].entry = yes >= 0 ? mt.info[usize(yes)].entry : mt.stack;
                    }
                    else if (no >= 0 && !mt.stack.Empty())
                        LeaveBlock(mt, no);
                    FlowBlock& block = mt.blocks[blockIndex];
                    block.end = end;
                    block.value = value;
                    block.succ[0] = yes;
                    block.succ[1] = no;
                };

                for (u32 i = first; i < last; i++)
                {
                    const Instruction in = mt.code[i];
                    u32 op = u32(in.opcode);
                    bool lastInBlock = i + 1 == last;
                    i32 next = i + 1 < mt.code.Size() ? target(mt.code[i + 1].offset) : -1;

                    // Argument, local and constant shortcuts share one path with their long forms.
                    bool handled = false;
                    if (op >= u32(Opcode::Ldarg0) && op <= u32(Opcode::Ldarg3))
                    {
                        u32 p = op - u32(Opcode::Ldarg0);
                        Push(mt, param(p), mt.fn.params[p].mode != ParamMode::In);
                        handled = true;
                    }
                    else if (op >= u32(Opcode::Ldloc0) && op <= u32(Opcode::Ldloc3))
                    {
                        Push(mt, local(op - u32(Opcode::Ldloc0)));
                        handled = true;
                    }
                    else if (op >= u32(Opcode::Stloc0) && op <= u32(Opcode::Stloc3))
                    {
                        ExprId value = Pop(mt).e;
                        Assign(mt, local(op - u32(Opcode::Stloc0)), value);
                        handled = true;
                    }
                    else if (op >= u32(Opcode::LdcI4M1) && op <= u32(Opcode::LdcI4_8))
                    {
                        Push(mt, ConstInt(mt, i64(op) - i64(Opcode::LdcI4_0), kInt));
                        handled = true;
                    }

                    if (!handled)
                    switch (in.opcode)
                    {
                    case Opcode::Nop: break;
                    case Opcode::LdargS: case Opcode::Ldarg:
                        Push(mt, param(u32(in.value)), mt.fn.params[u32(in.value)].mode != ParamMode::In);
                        break;
                    case Opcode::LdargaS: case Opcode::Ldarga: Push(mt, param(u32(in.value)), true); break;
                    case Opcode::StargS: case Opcode::Starg:
                    {
                        ExprId value = Pop(mt).e;
                        Assign(mt, param(u32(in.value)), value);
                        break;
                    }
                    case Opcode::LdlocS: case Opcode::Ldloc:
                    {
                        u32 slot = u32(in.value);
                        if (slot < mt.isRef.Size() && mt.isRef[slot])
                        {
                            // A ref local stands for the place it was set to.
                            if (!mt.refTarget[slot])
                                return Fail(mt.name, "a ref local is read before it is set");
                            Push(mt, mt.refTarget[slot], true);
                        }
                        else
                            Push(mt, local(slot));
                        break;
                    }
                    case Opcode::LdlocaS: case Opcode::Ldloca:
                        if (u32(in.value) < mt.isRef.Size() && mt.isRef[u32(in.value)])
                            return Fail(mt.name, "the address of a ref local is not available in shaders");
                        Push(mt, local(u32(in.value)), true);
                        break;
                    case Opcode::StlocS: case Opcode::Stloc:
                    {
                        u32 slot = u32(in.value);
                        Item item = Pop(mt);
                        if (slot < mt.isRef.Size() && mt.isRef[slot])
                        {
                            if (!item.address)
                                return Fail(mt.name, "a ref local must refer to a variable, a field or an element");
                            mt.refTarget[slot] = item.e;
                            break;
                        }
                        Assign(mt, local(slot), item.e);
                        break;
                    }
                    case Opcode::LdcI4S: case Opcode::LdcI4: Push(mt, ConstInt(mt, in.value, kInt)); break;
                    case Opcode::LdcR4: case Opcode::LdcR8: Push(mt, ConstFloat(mt, in.number)); break;
                    case Opcode::Dup:
                    {
                        Item top = Pop(mt);
                        ExprKind kind = mt.fn.exprs[top.e].kind;
                        bool simple = top.address || kind == ExprKind::ConstFloat || kind == ExprKind::ConstInt || kind == ExprKind::ConstBool ||
                                      kind == ExprKind::Local || kind == ExprKind::Param;
                        if (!simple)
                            Spill(mt, top);
                        mt.stack.Push(top);
                        mt.stack.Push(top);
                        break;
                    }
                    case Opcode::Pop: Pop(mt); break;

                    case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::Div: case Opcode::DivUn: case Opcode::Rem:
                    case Opcode::RemUn: case Opcode::And: case Opcode::Or: case Opcode::Xor: case Opcode::Shl: case Opcode::Shr:
                    case Opcode::ShrUn:
                    {
                        static constexpr Op kOps[] = {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Div, Op::Rem, Op::Rem,
                                                      Op::And, Op::Or,  Op::Xor, Op::Shl, Op::Shr, Op::Shr};
                        bool asUnsigned = in.opcode == Opcode::DivUn || in.opcode == Opcode::RemUn || in.opcode == Opcode::ShrUn;
                        ExprId y = Pop(mt).e, x = Pop(mt).e;
                        Push(mt, Arithmetic(mt, kOps[op - u32(Opcode::Add)], x, y, asUnsigned));
                        break;
                    }
                    case Opcode::Neg: case Opcode::Not:
                    {
                        ExprId x = Pop(mt).e;
                        Push(mt, MakeUnary(mt.fn, in.opcode == Opcode::Neg ? Op::Neg : Op::BitNot, x, TypeOf(mt, x)));
                        break;
                    }
                    case Opcode::ConvR4: case Opcode::ConvR8:
                    {
                        ExprId x = Pop(mt).e;
                        Push(mt, TypeOf(mt, x).base == BaseType::Float ? x : Cast(mt, x, kFloat));
                        break;
                    }
                    case Opcode::ConvRUn:
                    {
                        ExprId x = Pop(mt).e;
                        Push(mt, Cast(mt, Coerce(mt, x, kUInt), kFloat));
                        break;
                    }
                    case Opcode::ConvI4: case Opcode::ConvU4:
                    {
                        ExprId x = Pop(mt).e;
                        Type to = in.opcode == Opcode::ConvI4 ? kInt : kUInt;
                        Push(mt, TypeOf(mt, x) == to ? x : Cast(mt, x, to));
                        break;
                    }
                    case Opcode::Ceq: case Opcode::Cgt: case Opcode::CgtUn: case Opcode::Clt: case Opcode::CltUn:
                    {
                        ExprId y = Pop(mt).e, x = Pop(mt).e;
                        Op cmp = in.opcode == Opcode::Ceq ? Op::Eq : (in.opcode == Opcode::Cgt || in.opcode == Opcode::CgtUn) ? Op::Gt : Op::Lt;
                        Push(mt, Compare(mt, cmp, x, y, in.opcode == Opcode::CgtUn || in.opcode == Opcode::CltUn));
                        break;
                    }

                    case Opcode::Ldfld: case Opcode::Ldflda:
                    {
                        Item object = Pop(mt);
                        ExprId field;
                        if (!FieldExpr(mt, Token(u32(in.value)), object.e, field))
                            return false;
                        Push(mt, field, in.opcode == Opcode::Ldflda);
                        break;
                    }
                    case Opcode::Stfld:
                    {
                        ExprId value = Pop(mt).e;
                        Item object = Pop(mt);
                        ExprId field;
                        if (!FieldExpr(mt, Token(u32(in.value)), object.e, field))
                            return false;
                        Assign(mt, field, value);
                        break;
                    }
                    case Opcode::Ldsfld: case Opcode::Ldsflda:
                    {
                        Item item;
                        if (!StaticField(mt, Token(u32(in.value)), item))
                            return false;
                        Push(mt, item.e, in.opcode == Opcode::Ldsflda && item.address);
                        break;
                    }
                    // Reads through an address are the expression of the place itself.
                    case Opcode::Ldobj: case Opcode::LdindR4: case Opcode::LdindI4: case Opcode::LdindU4: case Opcode::LdindI1:
                    case Opcode::LdindU1: case Opcode::LdindR8:
                        Push(mt, Pop(mt).e);
                        break;
                    case Opcode::Stobj: case Opcode::StindR4: case Opcode::StindI4: case Opcode::StindI1: case Opcode::StindR8:
                    {
                        ExprId value = Pop(mt).e;
                        ExprId place = Pop(mt).e;
                        Assign(mt, place, value);
                        break;
                    }
                    case Opcode::Initobj:
                    {
                        ExprId place = Pop(mt).e;
                        Expr zero;
                        zero.kind = ExprKind::Construct;
                        zero.type = TypeOf(mt, place);
                        Assign(mt, place, AddExpr(mt.fn, Move(zero)));
                        break;
                    }
                    case Opcode::Call: case Opcode::Newobj:
                        if (!Call(mt, Token(u32(in.value)), in.opcode == Opcode::Newobj))
                            return false;
                        break;

                    case Opcode::Ret:
                    {
                        ExprId value = mt.fn.returnType.IsVoid() ? 0 : Coerce(mt, Pop(mt).e, mt.fn.returnType);
                        mt.stack.Clear();
                        finish(FlowBlock::End::Return, value, -1, -1);
                        return true;
                    }
                    case Opcode::Br: case Opcode::BrS:
                        finish(FlowBlock::End::Goto, 0, target(in.target), -1);
                        return true;
                    case Opcode::Brtrue: case Opcode::BrtrueS: case Opcode::Brfalse: case Opcode::BrfalseS:
                    {
                        ExprId condition = Coerce(mt, Pop(mt).e, kBool);
                        if (in.opcode == Opcode::Brfalse || in.opcode == Opcode::BrfalseS)
                            condition = MakeNot(mt.fn, condition);
                        finish(FlowBlock::End::Branch, condition, target(in.target), next);
                        return true;
                    }
                    case Opcode::Beq: case Opcode::BeqS: case Opcode::Bge: case Opcode::BgeS: case Opcode::Bgt: case Opcode::BgtS:
                    case Opcode::Ble: case Opcode::BleS: case Opcode::Blt: case Opcode::BltS: case Opcode::BneUn: case Opcode::BneUnS:
                    case Opcode::BgeUn: case Opcode::BgeUnS: case Opcode::BgtUn: case Opcode::BgtUnS: case Opcode::BleUn:
                    case Opcode::BleUnS: case Opcode::BltUn: case Opcode::BltUnS:
                    {
                        // Short and long forms are in the same order: beq bge bgt ble blt bne.un bge.un bgt.un ble.un blt.un.
                        static constexpr Op kOps[] = {Op::Eq, Op::Ge, Op::Gt, Op::Le, Op::Lt, Op::Ne, Op::Ge, Op::Gt, Op::Le, Op::Lt};
                        u32 k = op >= u32(Opcode::Beq) ? op - u32(Opcode::Beq) : op - u32(Opcode::BeqS);
                        ExprId y = Pop(mt).e, x = Pop(mt).e;
                        finish(FlowBlock::End::Branch, Compare(mt, kOps[k], x, y, k >= 6), target(in.target), next);
                        return true;
                    }
                    case Opcode::Switch:
                    {
                        // A chain of branches, with the cases that share a target tested together.
                        Item value = Pop(mt);
                        value.e = Coerce(mt, value.e, kInt);
                        if (mt.fn.exprs[value.e].kind != ExprKind::Local)
                            Spill(mt, value);
                        Array<i32> targets;
                        for (u32 c = 0; c < in.switchCount; c++)
                        {
                            i32 t = target(in.SwitchTarget(c));
                            if (!targets.Find(t))
                                targets.Push(t);
                        }
                        Array<Item> carried = mt.stack;
                        u32 current = blockIndex;
                        for (usize t = 0; t < targets.Size(); t++)
                        {
                            ExprId condition = 0;
                            for (u32 c = 0; c < in.switchCount; c++)
                            {
                                if (target(in.SwitchTarget(c)) != targets[t])
                                    continue;
                                ExprId test = MakeBinary(mt.fn, Op::Eq, value.e, ConstInt(mt, c, kInt), kBool);
                                condition = condition ? MakeBinary(mt.fn, Op::LogicalOr, condition, test, kBool) : test;
                            }
                            i32 rest = next;
                            if (t + 1 < targets.Size())
                            {
                                mt.blocks.Emplace();
                                Method::BlockInfo& chain = mt.info.Emplace();
                                chain.done = chain.hasEntry = true;
                                chain.entry = carried;
                                rest = i32(mt.blocks.Size() - 1);
                            }
                            u32 saved = blockIndex;
                            blockIndex = current;
                            mt.stack = carried;
                            finish(FlowBlock::End::Branch, condition, targets[t], rest);
                            blockIndex = saved;
                            current = u32(rest);
                        }
                        if (targets.Empty())
                            finish(FlowBlock::End::Goto, 0, next, -1);
                        return true;
                    }
                    default:
                        return Fail(mt.name, Format("the instruction {} is not available in shaders", OpcodeName(in.opcode)));
                    }
                    if (lastInBlock)
                    {
                        finish(FlowBlock::End::Goto, 0, next, -1);
                        return true;
                    }
                }
                return Fail(mt.name, "a block ends without a successor");
            }

            bool TranslateMethod(u32 row, const Array<Type>& typeArgs, Function& out)
            {
                Method mt;
                MethodInfo info = a.Method(row);
                mt.name = info.name;
                mt.typeArgs = typeArgs;
                MethodBody body;
                if (!ParseMethodSignature(info.signature, mt.sig) || !a.Body(row, body))
                    return Fail(mt.name, "the method has no body that can be read");
                if (body.hasExceptionRegions)
                    return Fail(mt.name, "try, catch and finally are not available in shaders");
                bool byRef = false;
                if (!MapType(mt.sig.returnType, &mt, mt.fn.returnType, byRef, mt.name) || byRef)
                    return Fail(mt.name, "the return type cannot be used in shaders");

                if (mt.sig.hasThis)
                {
                    i32 self = StructFor(a.DeclaringTypeOfMethod(row), mt.name);
                    if (self < 0)
                        return false;
                    Variable& v = mt.fn.params.Emplace();
                    v.name = "self";
                    v.type = Type::Struct(u32(self));
                    v.mode = ParamMode::InOut;
                }
                for (usize i = 0; i < mt.sig.params.Size(); i++)
                {
                    Variable& v = mt.fn.params.Emplace();
                    byRef = false;
                    if (!MapType(mt.sig.params[i], &mt, v.type, byRef, mt.name))
                        return Fail(mt.name, Format("parameter {} has a type that shaders cannot use", i + 1));
                    v.name = Format("a{}", i);
                    v.mode = byRef ? ParamMode::InOut : ParamMode::In;
                    for (u32 p = info.paramBegin; p < info.paramEnd; p++)
                    {
                        ParamInfo pi = a.Param(p);
                        if (pi.sequence != i + 1)
                            continue;
                        v.name = pi.name;
                        // Flag 2 is `out`, flag 1 is `in` (a read only reference).
                        if (byRef)
                            v.mode = (pi.flags & 2) ? ParamMode::Out : (pi.flags & 1) ? ParamMode::In : ParamMode::InOut;
                        v.semantic = ReadSemantic(Token(Table::Param, p), v.semanticIndex, v.flat);
                    }
                }
                if (!body.localsSignature.IsNull())
                {
                    Array<TypeSig> locals;
                    if (!body.localsSignature.Is(Table::StandAloneSig) || !ParseLocalsSignature(a.StandAloneSignature(body.localsSignature.Row()), locals))
                        return Fail(mt.name, "damaged local variable signature");
                    for (const TypeSig& s : locals)
                    {
                        Type t;
                        byRef = false;
                        if (!MapType(s, &mt, t, byRef, mt.name))
                            return Fail(mt.name, "a local variable has a type that shaders cannot use");
                        mt.isRef.Push(byRef ? 1 : 0);
                        mt.refTarget.Push(0);
                        NewLocal(mt, t, false);
                    }
                }

                // Decode and cut into blocks at branch targets and after branches.
                Array<u8> leader;
                leader.Resize(body.code.Size() + 1, u8(0));
                leader[0] = 1;
                Instruction in;
                for (u32 at = 0; at < body.code.Size();)
                {
                    if (!DecodeInstruction(body.code, at, in))
                        return Fail(mt.name, "unknown IL instruction");
                    mt.code.Push(in);
                    bool branch = in.operand == OperandKind::Branch8 || in.operand == OperandKind::Branch32;
                    if (branch || in.opcode == Opcode::Switch || in.opcode == Opcode::Ret)
                        leader[at] = 1;
                    if (branch && in.target < leader.Size())
                        leader[in.target] = 1;
                    for (u32 c = 0; c < in.switchCount; c++)
                        if (in.SwitchTarget(c) < leader.Size())
                            leader[in.SwitchTarget(c)] = 1;
                }
                mt.blockOfOffset.Resize(body.code.Size() + 1, -1);
                for (u32 i = 0; i < mt.code.Size(); i++)
                {
                    if (leader[mt.code[i].offset])
                    {
                        if (!mt.info.Empty())
                            mt.info.Back().last = i;
                        mt.blocks.Emplace();
                        mt.info.Emplace().first = i;
                        mt.blockOfOffset[mt.code[i].offset] = i32(mt.blocks.Size() - 1);
                    }
                }
                if (mt.info.Empty())
                    return Fail(mt.name, "the method is empty");
                mt.info.Back().last = u32(mt.code.Size());
                for (const Instruction& i : mt.code)
                    if ((i.operand == OperandKind::Branch8 || i.operand == OperandKind::Branch32) &&
                        (i.target >= mt.blockOfOffset.Size() || mt.blockOfOffset[i.target] < 0))
                        return Fail(mt.name, "a branch leaves the method");

                // Translate reachable blocks; a block is reached after one of its predecessors fixed its entry stack.
                Array<u32> work;
                work.Push(0);
                mt.info[0].hasEntry = true;
                while (!work.Empty())
                {
                    u32 b = work.Back();
                    work.Pop();
                    if (mt.info[b].done)
                        continue;
                    mt.info[b].done = true;
                    usize before = mt.blocks.Size();
                    if (!TranslateBlock(mt, b))
                        return false;
                    for (i32 s : mt.blocks[b].succ)
                        if (s >= 0)
                            work.Push(u32(s));
                    // Chain blocks of a switch are complete already, but their targets still need a visit.
                    for (usize extra = before; extra < mt.blocks.Size(); extra++)
                        for (i32 s : mt.blocks[extra].succ)
                            if (s >= 0)
                                work.Push(u32(s));
                }
                for (FlowBlock& block : mt.blocks)
                    for (i32& s : block.succ)
                        if (s >= i32(mt.blocks.Size()))
                            return Fail(mt.name, "damaged control flow");
                String flowError;
                if (!StructureFlow(mt.blocks, mt.fn, mt.fn.body, flowError))
                    return Fail(mt.name, flowError);
                out = Move(mt.fn);
                return true;
            }
        };
    }

    bool TranslateEntryPoint(const Assembly& assembly, u32 method, Stage stage, Module& out, String& error, const TranslateOptions& options)
    {
        out = Module();
        out.stage = stage;
        error.Clear();
        Translator t(assembly, out, error, options);
        Array<Type> none;
        i32 index = t.FunctionFor(method, none, assembly.Method(method).name);
        if (index < 0)
        {
            if (error.Empty())
                error = "translation failed";
            return false;
        }
        out.entry = u32(index);
        // Role of the return value: the attributes of the parameter row with sequence 0.
        MethodInfo info = assembly.Method(method);
        for (u32 p = info.paramBegin; p < info.paramEnd; p++)
            if (assembly.Param(p).sequence == 0)
            {
                bool flat;
                out.returnSemantic = t.ReadSemantic(Token(Table::Param, p), out.returnSemanticIndex, flat);
            }
        if (stage == Stage::Compute)
        {
            Array<AttributeArgument> size;
            if (!t.Has(Token(Table::MethodDef, method), "ComputeShaderAttribute", &size) || size.Size() != 3)
            {
                error = Format("{}: a compute shader needs [ComputeShader(x, y, z)]", info.name);
                return false;
            }
            for (usize i = 0; i < 3; i++)
                out.groupSize[i] = u32(size[i].integer);
            if (!out.functions[out.entry].returnType.IsVoid())
            {
                error = Format("{}: a compute shader returns nothing; it writes buffers", info.name);
                return false;
            }
        }
        Simplify(out);
        return true;
    }
}
