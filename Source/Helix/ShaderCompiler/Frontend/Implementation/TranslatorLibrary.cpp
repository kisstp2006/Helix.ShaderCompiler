#include <Helix/ShaderCompiler/Frontend/Implementation/Translator.h>

namespace helix::shadercompiler::frontend
{
    ExprId Translator::MakeIntrinsic(Method& mt, Intrinsic which, Type type, Span<const ExprId> args)
    {
        Expr e;
        e.kind = ExprKind::Intrinsic;
        e.intrinsic = which;
        e.type = type;
        for (ExprId arg : args)
            e.args.Push(arg);
        return AddExpr(mt.fn, Move(e));
    }

    bool Translator::TextureCall(Method& mt, StringView member, Array<Item>& args)
    {
        // A member reference with a damaged signature brings fewer arguments than the member takes.
        usize needed = member == "get_Size" ? 1 : member.StartsWith("SampleCmp") ? 4 : 3;
        if (args.Size() < needed)
            return Fail(mt.name, Format("texture member {} without its arguments", member));
        i32 global = ResourceOf(mt, args[0].e, "texture", [](const Global& g) { return g.type.IsTexture(); });
        if (global < 0)
            return false;
        u32 index = u32(global);
        Type element = m.globals[index].element;
        BaseType kind = m.globals[index].type.base;
        // Coordinates: two for 2D textures, three for arrays (the layer), volumes and cubes (a direction).
        u32 coordinates = kind == BaseType::Texture2D ? 2u : 3u;
        Expr e;
        e.kind = ExprKind::Intrinsic;
        e.type = element;
        e.args.Push(args[0].e);
        if (member == "Sample" || member == "SampleLevel")
        {
            if (element.base != BaseType::Float)
                return Fail(mt.name, Format("{} has integer texels and cannot be sampled; use Load", m.globals[index].name));
            e.intrinsic = member == "Sample" ? Intrinsic::TextureSample : Intrinsic::TextureSampleLevel;
            e.args.Push(Coerce(mt, args[2].e, Type::Vector(BaseType::Float, coordinates)));
            if (args.Size() > 3)
                e.args.Push(Coerce(mt, args[3].e, kFloat));
        }
        else if ((member == "SampleCmp" || member == "SampleCmpLevelZero") && args.Size() == 4)
        {
            // The sampler of the slot compares; the texture is a shadow map of single floats.
            if (element != kFloat || kind == BaseType::Texture3D)
                return Fail(mt.name, Format("{} must hold single floats in 2D, array or cube form to be sampled with a comparison",
                                            m.globals[index].name));
            m.globals[index].comparison = true;
            e.intrinsic = member == "SampleCmp" ? Intrinsic::TextureSampleCmp : Intrinsic::TextureSampleCmpLevelZero;
            e.type = kFloat;
            e.args.Push(Coerce(mt, args[2].e, Type::Vector(BaseType::Float, coordinates)));
            e.args.Push(Coerce(mt, args[3].e, kFloat));
        }
        else if (member == "Load" && args.Size() == 3 && kind != BaseType::TextureCube)
        {
            e.intrinsic = Intrinsic::TextureLoad;
            e.args.Push(Coerce(mt, args[1].e, Type::Vector(BaseType::Int, coordinates)));
            e.args.Push(Coerce(mt, args[2].e, kInt));
        }
        else if (member == "get_Size")
        {
            e.intrinsic = Intrinsic::TextureSize;
            e.type = Type::Vector(BaseType::Int, kind == BaseType::Texture2D || kind == BaseType::TextureCube ? 2u : 3u);
        }
        else
            return Fail(mt.name, Format("Texture2D.{} is not available in shaders", member));
        Push(mt, AddExpr(mt.fn, Move(e)));
        return true;
    }

    bool Translator::ArrayCall(Method& mt, StringView member, Array<Item>& args)
    {
        if (args.Empty() || TypeOf(mt, args[0].e).base != BaseType::Array)
            return Fail(mt.name, Format("{} is used on something that is not an inline array", member));
        ExprId array = args[0].e;
        const ArrayType& type = m.arrays[TypeOf(mt, array).structIndex];
        if (member == "InlineArrayAsSpan" || member == "InlineArrayAsReadOnlySpan")
        {
            Push(mt, array, true);
            return true;
        }
        if (member == "get_Length")
        {
            Push(mt, ConstInt(mt, type.length, kInt));
            return true;
        }
        ExprId index = 0;
        if (member == "InlineArrayFirstElementRef" || member == "InlineArrayFirstElementRefReadOnly")
            index = ConstInt(mt, 0, kInt);
        else if ((member == "InlineArrayElementRef" || member == "InlineArrayElementRefReadOnly" || member == "get_Item") && args.Size() == 2)
            index = Coerce(mt, args[1].e, kInt);
        else
            return Fail(mt.name, Format("{} of an inline array is not available in shaders; index the array in a for loop", member));
        Expr e;
        e.kind = ExprKind::Index;
        e.a = array;
        e.b = index;
        e.type = type.element;
        Push(mt, AddExpr(mt.fn, Move(e)), true);
        return true;
    }

    bool Translator::ImageCall(Method& mt, StringView member, Array<Item>& args)
    {
        if (args.Size() < (member == "set_Item" ? 3u : member == "get_Item" ? 2u : 1u))
            return Fail(mt.name, Format("storage texture member {} without its arguments", member));
        i32 global = ResourceOf(mt, args[0].e, "storage texture", [](const Global& g) { return g.type.base == BaseType::Image; });
        if (global < 0)
            return false;
        Global& g = m.globals[usize(global)];
        if (member == "get_Size")
        {
            Push(mt, MakeIntrinsic(mt, Intrinsic::ImageSize, Type::Vector(BaseType::Int, 2), Span<const ExprId>(&args[0].e, 1)));
            return true;
        }
        if ((member != "get_Item" && member != "set_Item") || args.Size() < 2)
            return Fail(mt.name, Format("RWTexture2D.{} is not available in shaders", member));
        Expr e;
        e.kind = ExprKind::Index;
        e.a = args[0].e;
        e.b = Coerce(mt, args[1].e, Type::Vector(BaseType::Int, 2));
        e.type = g.element;
        ExprId texel = AddExpr(mt.fn, Move(e));
        if (member == "set_Item")
        {
            Assign(mt, texel, args[2].e);
            return true;
        }
        // Direct3D 11 reads typed storage textures only in the single 32 bit formats.
        if (g.format != StorageFormat::R32F && g.format != StorageFormat::R32UI && g.format != StorageFormat::R32I)
            return Fail(mt.name, Format("storage texture {} is read, which needs the R32F, R32UI or R32I format", g.name));
        g.readable = true;
        Push(mt, texel);
        return true;
    }

    bool Translator::BufferCall(Method& mt, StringView member, Array<Item>& args)
    {
        if (args.Size() < (member == "set_Item" ? 3u : member == "get_Item" ? 2u : 1u))
            return Fail(mt.name, Format("buffer member {} without its arguments", member));
        i32 global = ResourceOf(mt, args[0].e, "buffer", [](const Global& g) { return g.kind == GlobalKind::Storage; });
        if (global < 0)
            return false;
        Type element = m.globals[usize(global)].element;
        bool writable = m.globals[usize(global)].writable;
        StringView name = m.globals[usize(global)].name;
        if (member == "get_Length")
        {
            Push(mt, MakeIntrinsic(mt, Intrinsic::BufferLength, kInt, Span<const ExprId>(&args[0].e, 1)));
            return true;
        }
        Expr e;
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

    bool Translator::LibraryCall(Method& mt, StringView typeName, StringView member, const MethodSig& sig, Array<Item>& args, bool isNew, bool& unknown)
    {
        if (args.Size() < sig.params.Size() + (sig.hasThis && !isNew ? 1u : 0u) || (args.Empty() && member != "Discard" && !isNew))
            return Fail(mt.name, Format("{}.{} without its arguments", typeName, member));
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
            {
                unknown = true;
                return false;
            }
            if (args.Size() - first != IntrinsicArity(e.intrinsic))
                return Fail(mt.name, Format("ShaderMath.{} takes {} arguments, the call has {}", member, IntrinsicArity(e.intrinsic), args.Size() - first));
            if (e.intrinsic == Intrinsic::InterlockedAdd)
            {
                if (ResourceOf(mt, args[0].e, "buffer", [](const Global& g) { return g.kind == GlobalKind::Storage; }) < 0)
                    return false;
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
            // The handle comes first unless a new one is made; damaged IL can call any member either way.
            auto takes = [&](usize count, bool constructs) {
                if (isNew == constructs && args.Size() == count)
                    return true;
                return Fail(mt.name, Format("{}.{} takes {} arguments, the call has {}", typeName, member, count, args.Size()));
            };
            if (member == ".ctor")
            {
                if (!takes(isNew ? 1 : 2, isNew))
                    return false;
                if (isNew)
                    Push(mt, Coerce(mt, args[0].e, kUInt));
                else
                    Assign(mt, args[0].e, args[1].e);
                return true;
            }
            if (member == "get_Index")
            {
                if (!takes(1, false))
                    return false;
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
                if (!takes(IntrinsicArity(k.intrinsic), false))
                    return false;
                Array<ExprId> operands;
                for (const Item& arg : args)
                    operands.Push(arg.e);
                ExprId value = MakeIntrinsic(mt, k.intrinsic, result, Span<const ExprId>(operands.Data(), operands.Size()));
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
        if (!LibraryType(typeName, self) || !self.IsNumeric())
        {
            unknown = true;
            return false;
        }

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
        // Conversions between library types (half to float and back, int to float, ...) are casts either way.
        if ((member == "op_Explicit" || member == "op_Implicit") && args.Size() == 1)
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
        if ((getter && args.Size() < 1) || (setter && args.Size() < 2))
            return Fail(mt.name, Format("{}.{} without its arguments", typeName, member));
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
        unknown = true;
        return false;
    }

    bool Translator::LibraryBody(Method& mt, Token method, const Array<Type>& typeArgs, StringView typeName, StringView member, Array<Item>& args, bool isNew)
    {
        u32 assembly = current, row = 0;
        if (method.Is(Table::MethodDef))
            row = method.Row();
        else if (!method.Is(Table::MemberRef) || !ResolveMethodRef(method, assembly, row))
        {
            bool loaded = false;
            for (const Assembly* r : assemblies)
                loaded = loaded || r->Name() == kLibrary;
            return Fail(mt.name, Format("{}.{} is not available in shaders{}", typeName, member,
                                        loaded ? "" : " (pass HelixEngine.Shaders in TranslateOptions::references to compile the methods of the library)"));
        }
        Array<CustomAttribute> attributes;
        assemblies[assembly]->Attributes(Token(Table::MethodDef, row), attributes);
        for (const CustomAttribute& c : attributes)
            if (c.typeNamespace == kLibrary && c.typeName == "IntrinsicAttribute")
                return Fail(mt.name, Format("{}.{} is an intrinsic the compiler does not know", typeName, member));
        if (isNew)
            return Fail(mt.name, Format("this constructor of {} is not available in shaders", typeName));
        return UserCall(mt, assembly, row, typeArgs, args);
    }
}
