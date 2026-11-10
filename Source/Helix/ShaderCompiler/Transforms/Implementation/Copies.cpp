#include <Helix/ShaderCompiler/Transforms/Implementation/Optimize.h>

namespace helix::shadercompiler::passes
{
    namespace
    {
        // Per local: the number of whole assignments (2 means two or more, or written in part or by a call) and the value.
        struct Writes
        {
            Array<u8> locals, params;
            Array<ExprId> value;
        };

        void CallWrites(const Module& module, const Function& f, ExprId id, Writes& w)
        {
            if (!id)
                return;
            const Expr& e = f.exprs[id];
            if (e.kind == ExprKind::Call)
                for (usize i = 0; i < e.args.Size() && i < module.functions[e.index].params.Size(); i++)
                    if (module.functions[e.index].params[i].mode != ParamMode::In)
                    {
                        // Written by the call: whatever variable the argument is rooted at.
                        ExprId root = e.args[i];
                        while (root && (f.exprs[root].kind == ExprKind::Field || f.exprs[root].kind == ExprKind::Swizzle ||
                                        f.exprs[root].kind == ExprKind::Index))
                            root = f.exprs[root].a;
                        if (f.exprs[root].kind == ExprKind::Local)
                            w.locals[f.exprs[root].index] = 2;
                        else if (f.exprs[root].kind == ExprKind::Param)
                            w.params[f.exprs[root].index] = 2;
                    }
            CallWrites(module, f, e.a, w);
            CallWrites(module, f, e.b, w);
            for (ExprId arg : e.args)
                CallWrites(module, f, arg, w);
        }

        Writes CountWrites(const Module& module, const Function& f)
        {
            Writes w;
            w.locals.Resize(f.locals.Size(), u8(0));
            w.params.Resize(f.params.Size(), u8(0));
            w.value.Resize(f.locals.Size(), ExprId(0));
            ForEachStatement(f.body, [&](const Stmt& s) {
                CallWrites(module, f, s.a, w);
                CallWrites(module, f, s.b, w);
                if (s.kind != StmtKind::Assign)
                    return;
                ExprId root = s.a;
                bool whole = true;
                while (root && (f.exprs[root].kind == ExprKind::Field || f.exprs[root].kind == ExprKind::Swizzle || f.exprs[root].kind == ExprKind::Index))
                {
                    root = f.exprs[root].a;
                    whole = false;
                }
                const Expr& r = f.exprs[root];
                if (r.kind == ExprKind::Param)
                    w.params[r.index] = 2;
                if (r.kind != ExprKind::Local)
                    return;
                u8& count = w.locals[r.index];
                count = u8(count || !whole ? 2 : 1);
                w.value[r.index] = s.b;
            });
            return w;
        }

        // A value that is the same at every point of the function: a parameter nothing writes or a uniform.
        bool Stable(const Module& module, const Function& f, const Writes& w, ExprId id)
        {
            const Expr& e = f.exprs[id];
            if (e.kind == ExprKind::Param)
                return w.params[e.index] == 0 && f.params[e.index].mode == ParamMode::In;
            return e.kind == ExprKind::Global && module.globals[e.index].kind == GlobalKind::Uniform;
        }

        bool Propagate(const Module& module, Function& f)
        {
            Writes w = CountWrites(module, f);
            Array<u8> replace;
            replace.Resize(f.locals.Size(), u8(0));
            bool any = false;
            for (usize l = 0; l < f.locals.Size(); l++)
                if (w.locals[l] == 1 && f.exprs[w.value[l]].type == f.locals[l].type && Stable(module, f, w, w.value[l]))
                {
                    replace[l] = 1;
                    any = true;
                }
            if (!any)
                return false;
            // Drop the assignments, then let every read of such a local read the value instead.
            auto drop = [&](auto&& self, Array<Stmt>& stmts) -> void {
                Array<Stmt> kept;
                for (Stmt& s : stmts)
                {
                    self(self, s.body);
                    self(self, s.elseBody);
                    bool whole;
                    i32 local = s.kind == StmtKind::Assign ? RootLocal(f, s.a, whole) : -1;
                    if (local >= 0 && replace[usize(local)])
                        continue;
                    kept.Push(Move(s));
                }
                stmts = Move(kept);
            };
            drop(drop, f.body);
            for (Expr& e : f.exprs)
                if (e.kind == ExprKind::Local && replace[e.index])
                    e = f.exprs[w.value[e.index]];
            return true;
        }
    }

    bool PropagateCopies(Module& module, String&)
    {
        for (Function& f : module.functions)
            while (Propagate(module, f))
            {
            }
        return true;
    }
}
