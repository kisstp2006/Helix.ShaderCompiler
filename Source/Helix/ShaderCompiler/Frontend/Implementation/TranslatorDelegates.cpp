#include <Helix/ShaderCompiler/Frontend/Implementation/Translator.h>

namespace helix::shadercompiler::frontend
{
    bool Translator::IsLambdaHolder(u32 typeRow) const
    {
        StringView name = a->TypeDef(typeRow).name;
        return name == "<>c" || name == "<>O";
    }

    bool Translator::IsDelegateTypeToken(Token t) const
    {
        if (t.Is(Table::TypeSpec))
        {
            TypeSig spec;
            return ParseTypeSpec(a->TypeSpecSignature(t.Row()), spec) && spec.kind == ElementType::GenericInst && IsDelegateTypeToken(spec.token);
        }
        StringView ns, name;
        if (!a->TypeName(t, ns, name))
            return false;
        if (ns == "System" && (name.StartsWith("Func`") || name == "Action" || name.StartsWith("Action`")))
            return true;
        if (!t.Is(Table::TypeDef))
            return false;
        StringView baseNs, baseName;
        return a->TypeName(a->TypeDef(t.Row()).extends, baseNs, baseName) && baseNs == "System" && baseName == "MulticastDelegate";
    }

    bool Translator::IsDelegateType(const TypeSig& sig) const
    {
        return (sig.kind == ElementType::GenericInst || sig.kind == ElementType::Class) && IsDelegateTypeToken(sig.token);
    }

    bool Translator::MethodHasThis(u32 assembly, u32 row) const
    {
        MethodSig sig;
        return ParseMethodSignature(assemblies[assembly]->Method(row).signature, sig) && sig.hasThis;
    }

    bool Translator::SameDelegate(const Method& mt, i32 x, i32 y)
    {
        const DelegateInfo& p = mt.delegates[usize(x)];
        const DelegateInfo& q = mt.delegates[usize(y)];
        if (p.assembly != q.assembly || p.target != q.target || p.typeArgs.Size() != q.typeArgs.Size() || (p.closure == 0) != (q.closure == 0))
            return false;
        for (usize i = 0; i < p.typeArgs.Size(); i++)
            if (!(p.typeArgs[i] == q.typeArgs[i]))
                return false;
        if (!p.closure)
            return true;
        const Expr& ca = mt.fn.exprs[p.closure];
        const Expr& cb = mt.fn.exprs[q.closure];
        return ca.kind == cb.kind && ca.index == cb.index;
    }

    bool Translator::DelegateCall(Method& mt, StringView member, Array<Item>& args, bool isNew)
    {
        if (isNew)
        {
            if (args.Size() != 2 || args[1].kind != ItemKind::Function)
                return Fail(mt.name, "a delegate must be made of a lambda, a local function or a static method");
            DelegateInfo d = mt.delegates[usize(args[1].index)];
            bool hasThis = MethodHasThis(d.assembly, d.target);
            if (args[0].kind == ItemKind::Value && hasThis && TypeOf(mt, args[0].e).base == BaseType::Struct)
            {
                // The closure must be a variable: the lambda writes the captured variables back to it.
                ExprId closure = args[0].e;
                ExprKind kind = mt.fn.exprs[closure].kind;
                if (kind != ExprKind::Local && kind != ExprKind::Param)
                {
                    Type t = TypeOf(mt, closure);
                    ExprId place = Leaf(mt, ExprKind::Local, NewLocal(mt, t, false), t);
                    Assign(mt, place, closure);
                    closure = place;
                }
                d.closure = closure;
            }
            else if (args[0].kind != ItemKind::Null || (hasThis && !IsLambdaHolder(a->DeclaringTypeOfMethod(d.target))))
                return Fail(mt.name, "a delegate of an instance method is not available in shaders; use a lambda or a static method");
            mt.delegates.Push(Move(d));
            mt.stack.Push({0, false, ItemKind::Delegate, i32(mt.delegates.Size() - 1)});
            return true;
        }
        if (member != "Invoke")
            return Fail(mt.name, Format("{} of a delegate is not available in shaders; call the delegate", member));
        if (args.Empty() || args[0].kind != ItemKind::Delegate)
            return Fail(mt.name, "only delegates known when the shader compiles can be called: a lambda, a local function or a static "
                                 "method set in this method or passed by the caller");
        DelegateInfo d = mt.delegates[usize(args[0].index)];
        Array<Item> callArgs;
        if (MethodHasThis(d.assembly, d.target))
            callArgs.Push(d.closure ? Item{d.closure, true} : Item{0, false, ItemKind::Null});
        for (usize i = 1; i < args.Size(); i++)
            callArgs.Push(args[i]);
        return UserCall(mt, d.assembly, d.target, d.typeArgs, callArgs);
    }
}
