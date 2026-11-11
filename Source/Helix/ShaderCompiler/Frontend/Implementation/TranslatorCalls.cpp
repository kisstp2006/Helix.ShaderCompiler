#include <Helix/ShaderCompiler/Frontend/Implementation/Translator.h>

namespace helix::shadercompiler::frontend
{
    i32 Translator::FunctionFor(u32 methodRow, const Array<Type>& typeArgs, StringView caller, const Array<Binding>& bindings)
    {
        String key = Format("{}:{}", current, methodRow);
        for (const Type& t : typeArgs)
            FormatAppend(key, ",{}.{}.{}.{}", u32(t.base), u32(t.rows), u32(t.cols), u32(t.structIndex));
        for (const Binding& b : bindings)
        {
            FormatAppend(key, ";d{}:{}:{}:{}", b.arg, b.assembly, b.target, b.closure);
            for (const Type& t : b.typeArgs)
                FormatAppend(key, ",{}.{}.{}.{}", u32(t.base), u32(t.rows), u32(t.cols), u32(t.structIndex));
        }
        for (const Compiled& c : compiled)
            if (c.key == key)
                return c.index;
        for (const String& b : building)
            if (b == key)
            {
                Fail(caller, Format("{} is recursive, shaders cannot recurse", a->Method(methodRow).name));
                return -1;
            }
        building.Push(key);
        Function fn;
        if (!TranslateMethod(methodRow, typeArgs, bindings, fn))
            return -1;
        building.Pop();
        fn.name = Format("f_{}_{}", a->Method(methodRow).name, m.functions.Size());
        m.functions.Push(Move(fn));
        compiled.Push({Move(key), i32(m.functions.Size() - 1)});
        return i32(m.functions.Size() - 1);
    }

    i32 Translator::FunctionIn(u32 assembly, u32 methodRow, const Array<Type>& typeArgs, StringView caller, const Array<Binding>& bindings)
    {
        const Assembly* saved = a;
        u32 savedIndex = current;
        a = assemblies[assembly];
        current = assembly;
        i32 index = FunctionFor(methodRow, typeArgs, caller, bindings);
        a = saved;
        current = savedIndex;
        return index;
    }

    bool Translator::ResolveType(Token type, u32& assembly, u32& typeRow, u32 depth) const
    {
        if (type.Is(Table::TypeDef))
        {
            assembly = current;
            typeRow = type.Row();
            return true;
        }
        if (!type.Is(Table::TypeRef))
            return false;
        StringView ns, name;
        if (!a->TypeName(type, ns, name))
            return false;
        Token scope = a->CodedCell(Table::TypeRef, type.Row(), 0);
        if (scope.Is(Table::TypeRef))
        {
            // Types nest far less deep than this.
            u32 outer = 0;
            if (depth >= 32 || !ResolveType(scope, assembly, outer, depth + 1))
                return false;
            typeRow = assemblies[assembly]->FindNestedType(outer, name);
            return typeRow != 0;
        }
        if (!scope.Is(Table::AssemblyRef))
            return false;
        // Column 6 of AssemblyRef is the name.
        StringView reference = a->GetString(a->Cell(Table::AssemblyRef, scope.Row(), 6));
        for (usize i = 0; i < assemblies.Size(); i++)
            if (assemblies[i]->Name() == reference)
            {
                assembly = u32(i);
                typeRow = assemblies[i]->FindType(ns, name);
                return typeRow != 0;
            }
        return false;
    }

    bool Translator::SameType(const Assembly& x, const TypeSig& s, const Assembly& y, const TypeSig& t)
    {
        if (s.kind != t.kind || s.index != t.index || s.args.Size() != t.args.Size())
            return false;
        if (s.kind == ElementType::ValueType || s.kind == ElementType::Class || s.kind == ElementType::GenericInst)
        {
            StringView ns1, n1, ns2, n2;
            if (!x.TypeName(s.token, ns1, n1) || !y.TypeName(t.token, ns2, n2) || ns1 != ns2 || n1 != n2)
                return false;
        }
        for (usize i = 0; i < s.args.Size(); i++)
            if (!SameType(x, s.args[i], y, t.args[i]))
                return false;
        return true;
    }

    bool Translator::ResolveMethodRef(Token token, u32& assembly, u32& row) const
    {
        MemberRefInfo ref = a->MemberRef(token.Row());
        u32 typeRow = 0;
        MethodSig want;
        if (!ResolveType(ref.parent, assembly, typeRow) || !ParseMethodSignature(ref.signature, want))
            return false;
        const Assembly& target = *assemblies[assembly];
        TypeDefInfo t = target.TypeDef(typeRow);
        for (u32 r = t.methodBegin; r < t.methodEnd; r++)
        {
            MethodInfo info = target.Method(r);
            MethodSig have;
            if (info.name != ref.name || !ParseMethodSignature(info.signature, have) || have.hasThis != want.hasThis ||
                have.genericParamCount != want.genericParamCount || have.params.Size() != want.params.Size() ||
                !SameType(*a, want.returnType, target, have.returnType))
                continue;
            bool same = true;
            for (usize i = 0; same && i < want.params.Size(); i++)
                same = SameType(*a, want.params[i], target, have.params[i]);
            if (same)
            {
                row = r;
                return true;
            }
        }
        return false;
    }

    bool Translator::Call(Method& mt, Token token, bool isNew)
    {
        Array<Type> typeArgs;
        if (token.Is(Table::MethodSpec))
        {
            Array<TypeSig> sigs;
            if (!ParseMethodSpec(a->GetBlob(a->Cell(Table::MethodSpec, token.Row(), 1)), sigs))
                return Fail(mt.name, "damaged generic method instantiation");
            for (const TypeSig& s : sigs)
            {
                bool byRef = false;
                if (!MapType(s, &mt, typeArgs.Emplace(), byRef, mt.name))
                    return Fail(mt.name, "a generic method is used with a type that shaders cannot use");
            }
            token = a->CodedCell(Table::MethodSpec, token.Row(), 0);
        }
        MethodSig sig;
        StringView ns, typeName, member;
        bool user = token.Is(Table::MethodDef), buffer = false, texture = false, image = false, span = false, inlineArray = false,
             delegate = false;
        if (user)
        {
            MethodInfo info = a->Method(token.Row());
            member = info.name;
            if (!ParseMethodSignature(info.signature, sig))
                return Fail(mt.name, "damaged method signature");
            // Helpers the C# compiler adds for inline arrays.
            inlineArray = a->TypeDef(a->DeclaringTypeOfMethod(token.Row())).name == "<PrivateImplementationDetails>" && member.StartsWith("InlineArray");
            delegate = IsDelegateTypeToken(Token(Table::TypeDef, a->DeclaringTypeOfMethod(token.Row())));
            // Inside the library its own types and functions are still the intrinsic ones.
            if (a->TypeName(Token(Table::TypeDef, a->DeclaringTypeOfMethod(token.Row())), ns, typeName) && ns == kLibrary)
                user = false;
        }
        else if (token.Is(Table::MemberRef))
        {
            MemberRefInfo ref = a->MemberRef(token.Row());
            member = ref.name;
            TypeSig spec;
            bool generic = ref.parent.Is(Table::TypeSpec) && ParseTypeSpec(a->TypeSpecSignature(ref.parent.Row()), spec);
            if (IsDelegateTypeToken(ref.parent))
            {
                delegate = true;
                if (!ParseMethodSignature(ref.signature, sig))
                    return Fail(mt.name, "damaged method signature");
            }
            else if (generic && IsSpanType(spec))
            {
                span = true;
                if (!ParseMethodSignature(ref.signature, sig))
                    return Fail(mt.name, "damaged method signature");
            }
            else if (generic && IsImageType(spec))
            {
                image = true;
                if (!ParseMethodSignature(ref.signature, sig))
                    return Fail(mt.name, "damaged method signature");
            }
            else if (generic && TextureType(spec) != BaseType::Void)
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
            else if (!ParseMethodSignature(ref.signature, sig) || !a->TypeName(ref.parent, ns, typeName))
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

        if (delegate)
            return DelegateCall(mt, member, args, isNew);
        // Delegates go to methods of the shader assembly and of included assemblies.
        bool include = !user && !ns.Empty() && ns != kLibrary;
        for (const Item& item : args)
            if (item.kind != ItemKind::Value && ((!user && !include) || item.kind == ItemKind::Function))
                return Fail(mt.name, Format("a delegate is passed to {}; delegates may only be called or passed to methods of the shader", member));
        if (buffer)
            return BufferCall(mt, member, args);
        if (inlineArray || span)
            return ArrayCall(mt, member, args);
        if (texture)
            return TextureCall(mt, member, args);
        if (image)
            return ImageCall(mt, member, args);
        if (!user)
        {
            if (ns != kLibrary)
            {
                // A method of an included assembly.
                u32 assembly = current, row = 0;
                if (!token.Is(Table::MemberRef) || !ResolveMethodRef(token, assembly, row))
                    return Fail(mt.name, Format("{}.{}.{} is not available in shaders{}", ns, typeName, member,
                                                ns == "System" || ns.StartsWith("System.") ? "" : " (is its assembly among TranslateOptions::references?)"));
                if (isNew)
                    return Fail(mt.name, Format("constructor {} of a user type: use a static method or set the fields", typeName));
                return UserCall(mt, assembly, row, typeArgs, args);
            }
            bool unknown = false;
            if (LibraryCall(mt, typeName, member, sig, args, isNew, unknown))
                return true;
            return unknown && LibraryBody(mt, token, typeArgs, typeName, member, args, isNew);
        }
        if (isNew && IsClosureType(a->DeclaringTypeOfMethod(token.Row())))
        {
            // A new closure: its fields are the captured variables, set by the code that follows.
            i32 closure = StructFor(a->DeclaringTypeOfMethod(token.Row()), mt.name);
            if (closure < 0)
                return false;
            Expr zero;
            zero.kind = ExprKind::Construct;
            zero.type = Type::Struct(u32(closure));
            Push(mt, AddExpr(mt.fn, Move(zero)));
            return true;
        }
        if (isNew)
            return Fail(mt.name, Format("constructor {} of a user type: use a static method or set the fields", member));
        return UserCall(mt, current, token.Row(), typeArgs, args);
    }

    bool Translator::UserCall(Method& mt, u32 assembly, u32 row, const Array<Type>& typeArgs, Array<Item>& args)
    {
        Array<Binding> bindings;
        for (usize i = 0; i < args.Size(); i++)
        {
            if (args[i].kind != ItemKind::Delegate)
                continue;
            const DelegateInfo& d = mt.delegates[usize(args[i].index)];
            Binding& b = bindings.Emplace();
            b.arg = u32(i);
            b.assembly = d.assembly;
            b.target = d.target;
            b.typeArgs = d.typeArgs;
            b.closure = d.closure ? i32(TypeOf(mt, d.closure).structIndex) : -1;
        }
        i32 index = assembly == current ? FunctionFor(row, typeArgs, mt.name, bindings) : FunctionIn(assembly, row, typeArgs, mt.name, bindings);
        if (index < 0)
            return false;
        const Function& callee = m.functions[usize(index)];
        Expr e;
        e.kind = ExprKind::Call;
        e.index = u32(index);
        e.type = callee.returnType;
        bool writes = false;
        usize p = 0;
        for (usize i = 0; i < args.Size(); i++)
        {
            ExprId value = args[i].e;
            if (args[i].kind == ItemKind::Null)
                continue;
            if (args[i].kind == ItemKind::Delegate)
            {
                value = mt.delegates[usize(args[i].index)].closure;
                if (!value)
                    continue;
            }
            if (p >= callee.params.Size())
                return Fail(mt.name, "the arguments of a call do not match its parameters");
            ParamMode mode = callee.params[p].mode;
            writes = writes || mode != ParamMode::In;
            if (mode != ParamMode::In)
                MarkWritten(mt, value);
            e.args.Push(mode == ParamMode::In ? Coerce(mt, value, callee.params[p].type) : value);
            p++;
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
}
