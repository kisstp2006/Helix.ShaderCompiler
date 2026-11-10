#include <Helix/ShaderCompiler/Transforms/Implementation/Optimize.h>

namespace helix::shadercompiler::passes
{
    bool IsConstant(const Expr& e) { return e.kind == ExprKind::ConstFloat || e.kind == ExprKind::ConstInt || e.kind == ExprKind::ConstBool; }

    i32 RootLocal(const Function& function, ExprId place, bool& whole)
    {
        whole = true;
        while (place)
        {
            const Expr& e = function.exprs[place];
            if (e.kind == ExprKind::Local)
                return i32(e.index);
            if (e.kind != ExprKind::Field && e.kind != ExprKind::Swizzle && e.kind != ExprKind::Index)
                return -1;
            whole = false;
            place = e.a;
        }
        return -1;
    }

    bool HasEffects(const Module& module, const Function& function, ExprId id, const Array<u8>& pure)
    {
        if (!id)
            return false;
        const Expr& e = function.exprs[id];
        if (e.kind == ExprKind::Intrinsic && (e.intrinsic == Intrinsic::InterlockedAdd || e.intrinsic == Intrinsic::BindlessStore))
            return true;
        if (e.kind == ExprKind::Call)
        {
            if (e.index >= pure.Size() || !pure[e.index])
                return true;
            for (const Variable& p : module.functions[e.index].params)
                if (p.mode != ParamMode::In)
                    return true;
        }
        if (HasEffects(module, function, e.a, pure) || HasEffects(module, function, e.b, pure))
            return true;
        for (ExprId arg : e.args)
            if (HasEffects(module, function, arg, pure))
                return true;
        return false;
    }

    Array<u8> PureFunctions(const Module& module)
    {
        Array<u8> pure;
        // Callees come before their callers.
        for (usize i = 0; i < module.functions.Size(); i++)
        {
            const Function& f = module.functions[i];
            bool clean = true;
            ForEachStatement(f.body, [&](const Stmt& s) {
                if (s.kind == StmtKind::Discard)
                    clean = false;
                if (s.kind == StmtKind::Assign)
                {
                    ExprId root = s.a;
                    while (root && (f.exprs[root].kind == ExprKind::Field || f.exprs[root].kind == ExprKind::Swizzle || f.exprs[root].kind == ExprKind::Index))
                        root = f.exprs[root].a;
                    const Expr& r = f.exprs[root];
                    if (r.kind == ExprKind::Global || (r.kind == ExprKind::Param && f.params[r.index].mode != ParamMode::In))
                        clean = false;
                }
                clean = clean && !HasEffects(module, f, s.a, pure) && !HasEffects(module, f, s.b, pure);
            });
            pure.Push(clean ? 1 : 0);
        }
        return pure;
    }

    namespace
    {
        // Marks the locals and parameters an expression reads.
        void Reads(const Function& f, ExprId id, Array<u8>& locals, Array<u8>& params)
        {
            if (!id)
                return;
            const Expr& e = f.exprs[id];
            if (e.kind == ExprKind::Local)
                locals[e.index] = 1;
            else if (e.kind == ExprKind::Param)
                params[e.index] = 1;
            Reads(f, e.a, locals, params);
            Reads(f, e.b, locals, params);
            for (ExprId arg : e.args)
                Reads(f, arg, locals, params);
        }

        // What a statement reads: all of its expressions, but of an assignment target only the subscripts and not the
        // variable written (a parameter written counts as read, it stays).
        void StatementReads(const Function& f, const Stmt& s, Array<u8>& locals, Array<u8>& params)
        {
            if (s.kind != StmtKind::Assign)
            {
                Reads(f, s.a, locals, params);
                return;
            }
            Reads(f, s.b, locals, params);
            for (ExprId place = s.a; place;)
            {
                const Expr& e = f.exprs[place];
                if (e.kind == ExprKind::Index)
                    Reads(f, e.b, locals, params);
                if (e.kind == ExprKind::Param)
                    params[e.index] = 1;
                if (e.kind != ExprKind::Field && e.kind != ExprKind::Swizzle && e.kind != ExprKind::Index)
                    break;
                place = e.a;
            }
        }

        // Drops assignments to locals that are never read; returns whether one went.
        bool DropDeadStores(const Module& module, Function& f, Array<Stmt>& stmts, const Array<u8>& read, const Array<u8>& pure)
        {
            bool changed = false;
            Array<Stmt> kept;
            for (Stmt& s : stmts)
            {
                changed = DropDeadStores(module, f, s.body, read, pure) || changed;
                changed = DropDeadStores(module, f, s.elseBody, read, pure) || changed;
                bool whole;
                i32 local = s.kind == StmtKind::Assign ? RootLocal(f, s.a, whole) : -1;
                if (local >= 0 && !read[usize(local)] && !HasEffects(module, f, s.b, pure))
                {
                    changed = true;
                    continue;
                }
                kept.Push(Move(s));
            }
            stmts = Move(kept);
            return changed;
        }

        void Neutralize(Expr& e)
        {
            // Expressions of removed statements stay in the arena; they must not point at what is gone.
            e = Expr();
            e.kind = ExprKind::ConstInt;
            e.type = kInt;
        }

        void RemoveUnusedLocals(Function& f)
        {
            Array<u8> used, params;
            used.Resize(f.locals.Size(), u8(0));
            params.Resize(f.params.Size(), u8(0));
            ForEachStatement(f.body, [&](const Stmt& s) {
                Reads(f, s.a, used, params);
                Reads(f, s.b, used, params);
            });
            constexpr u32 kGone = ~0u;
            Array<u32> map;
            Array<Variable> kept;
            for (usize i = 0; i < f.locals.Size(); i++)
            {
                map.Push(used[i] ? u32(kept.Size()) : kGone);
                if (used[i])
                    kept.Push(Move(f.locals[i]));
            }
            f.locals = Move(kept);
            for (Expr& e : f.exprs)
                if (e.kind == ExprKind::Local)
                {
                    if (map[e.index] == kGone)
                        Neutralize(e);
                    else
                        e.index = map[e.index];
                }
        }

        // Parameters of helper functions that nothing reads, when no caller computes an effect for them.
        void RemoveUnusedParameters(Module& module, const Array<u8>& pure)
        {
            for (usize callee = 0; callee < module.functions.Size(); callee++)
            {
                if (callee == module.entry)
                    continue;
                Function& f = module.functions[callee];
                Array<u8> locals, read;
                locals.Resize(f.locals.Size(), u8(0));
                read.Resize(f.params.Size(), u8(0));
                ForEachStatement(f.body, [&](const Stmt& s) { StatementReads(f, s, locals, read); });
                for (usize p = 0; p < f.params.Size(); p++)
                    if (f.params[p].mode != ParamMode::In)
                        read[p] = 1;
                // An argument with an effect keeps its parameter.
                for (const Function& caller : module.functions)
                    ForEachStatement(caller.body, [&](const Stmt& s) {
                        auto visit = [&](auto&& self, ExprId id) -> void {
                            if (!id)
                                return;
                            const Expr& e = caller.exprs[id];
                            if (e.kind == ExprKind::Call && e.index == callee)
                                for (usize i = 0; i < e.args.Size() && i < read.Size(); i++)
                                    if (HasEffects(module, caller, e.args[i], pure))
                                        read[i] = 1;
                            self(self, e.a);
                            self(self, e.b);
                            for (ExprId arg : e.args)
                                self(self, arg);
                        };
                        visit(visit, s.a);
                        visit(visit, s.b);
                    });
                bool all = true;
                for (u8 r : read)
                    all = all && r;
                if (all)
                    continue;
                constexpr u32 kGone = ~0u;
                Array<u32> map;
                Array<Variable> kept;
                for (usize p = 0; p < f.params.Size(); p++)
                {
                    map.Push(read[p] ? u32(kept.Size()) : kGone);
                    if (read[p])
                        kept.Push(Move(f.params[p]));
                }
                f.params = Move(kept);
                for (Expr& e : f.exprs)
                    if (e.kind == ExprKind::Param)
                    {
                        if (map[e.index] == kGone)
                            Neutralize(e);
                        else
                            e.index = map[e.index];
                    }
                for (Function& caller : module.functions)
                    for (Expr& e : caller.exprs)
                        if (e.kind == ExprKind::Call && e.index == callee && e.args.Size() == map.Size())
                        {
                            Array<ExprId> args;
                            for (usize i = 0; i < e.args.Size(); i++)
                                if (map[i] != kGone)
                                    args.Push(e.args[i]);
                            e.args = Move(args);
                        }
            }
        }
    }

    bool RemoveDeadCode(Module& module, String&)
    {
        Array<u8> pure = PureFunctions(module);
        for (Function& f : module.functions)
        {
            for (;;)
            {
                Array<u8> read, params;
                read.Resize(f.locals.Size(), u8(0));
                params.Resize(f.params.Size(), u8(0));
                ForEachStatement(f.body, [&](const Stmt& s) { StatementReads(f, s, read, params); });
                // A call writing a local through an out parameter reads it too, as far as this pass is concerned.
                ForEachStatement(f.body, [&](const Stmt& s) {
                    auto visit = [&](auto&& self, ExprId id) -> void {
                        if (!id)
                            return;
                        const Expr& e = f.exprs[id];
                        if (e.kind == ExprKind::Call)
                            for (ExprId arg : e.args)
                                Reads(f, arg, read, params);
                        self(self, e.a);
                        self(self, e.b);
                        for (ExprId arg : e.args)
                            self(self, arg);
                    };
                    visit(visit, s.a);
                    visit(visit, s.b);
                });
                if (!DropDeadStores(module, f, f.body, read, pure))
                    break;
            }
            RemoveUnusedLocals(f);
        }
        RemoveUnusedParameters(module, pure);
        return true;
    }
}
