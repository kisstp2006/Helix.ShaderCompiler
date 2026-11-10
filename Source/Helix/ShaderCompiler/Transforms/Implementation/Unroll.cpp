#include <Helix/ShaderCompiler/Transforms/Implementation/Optimize.h>

namespace helix::shadercompiler::passes
{
    namespace
    {
        // Without [Unroll] a loop is unrolled when it is short and its body small; with it up to this many times.
        constexpr u32 kAutoTrips = 8, kAutoStatements = 64, kMaxTrips = 256;

        u32 CountStatements(const Array<Stmt>& stmts)
        {
            u32 n = 0;
            ForEachStatement(stmts, [&](const Stmt&) { n++; });
            return n;
        }

        // A loop of the form the structuring gives a for loop:
        //   i = start; loop { if (i op limit) { body; i = i + step; continue } else break }
        struct Counted
        {
            u32 local = 0;
            i64 start = 0, limit = 0, step = 0;
            Op op = Op::Lt;
        };

        bool ConstantInt(const Function& f, ExprId id, i64& out)
        {
            const Expr& e = f.exprs[id];
            if (e.kind != ExprKind::ConstInt || (e.type != kInt && e.type != kUInt))
                return false;
            out = e.integer;
            return true;
        }

        bool IsLocal(const Function& f, ExprId id, u32 local) { return f.exprs[id].kind == ExprKind::Local && f.exprs[id].index == local; }

        // Whether a statement list writes the local anywhere, or leaves its loop early with continue (break is handled).
        bool Touches(const Module& m, const Function& f, const Array<Stmt>& stmts, u32 local, u32 depth, bool& continues)
        {
            for (const Stmt& s : stmts)
            {
                if (s.kind == StmtKind::Continue && depth == 0)
                    continues = true;
                bool whole;
                if (s.kind == StmtKind::Assign && RootLocal(f, s.a, whole) == i32(local))
                    return true;
                // A call that writes it through an out parameter.
                bool written = false;
                auto visit = [&](auto&& self, ExprId id) -> void {
                    if (!id || written)
                        return;
                    const Expr& e = f.exprs[id];
                    if (e.kind == ExprKind::Call)
                        for (usize i = 0; i < e.args.Size(); i++)
                            if (m.functions[e.index].params[i].mode != ParamMode::In && RootLocal(f, e.args[i], whole) == i32(local))
                                written = true;
                    self(self, e.a);
                    self(self, e.b);
                    for (ExprId arg : e.args)
                        self(self, arg);
                };
                visit(visit, s.a);
                visit(visit, s.b);
                if (written)
                    return true;
                u32 inner = depth + (s.kind == StmtKind::Loop ? 1 : 0);
                if (Touches(m, f, s.body, local, inner, continues) || Touches(m, f, s.elseBody, local, inner, continues))
                    return true;
            }
            return false;
        }

        bool Recognize(const Module& m, const Function& f, const Stmt& init, const Stmt& loop, Counted& out, Array<Stmt>& body)
        {
            if (init.kind != StmtKind::Assign || loop.kind != StmtKind::Loop || f.exprs[init.a].kind != ExprKind::Local || loop.body.Size() != 1)
                return false;
            out.local = f.exprs[init.a].index;
            if (!ConstantInt(f, init.b, out.start))
                return false;
            const Stmt& test = loop.body[0];
            if (test.kind != StmtKind::If || test.elseBody.Size() != 1 || test.elseBody[0].kind != StmtKind::Break || test.body.Size() < 2)
                return false;
            const Expr& condition = f.exprs[test.a];
            if (condition.kind != ExprKind::Binary || !IsLocal(f, condition.a, out.local) || !ConstantInt(f, condition.b, out.limit))
                return false;
            out.op = condition.op;
            if (out.op != Op::Lt && out.op != Op::Le && out.op != Op::Gt && out.op != Op::Ge && out.op != Op::Ne)
                return false;
            const Stmt& last = test.body[test.body.Size() - 1];
            const Stmt& step = test.body[test.body.Size() - 2];
            if (last.kind != StmtKind::Continue || step.kind != StmtKind::Assign || !IsLocal(f, step.a, out.local))
                return false;
            const Expr& add = f.exprs[step.b];
            if (add.kind != ExprKind::Binary || add.op != Op::Add || !IsLocal(f, add.a, out.local) || !ConstantInt(f, add.b, out.step) || out.step == 0)
                return false;
            body.Clear();
            for (usize i = 0; i + 2 < test.body.Size(); i++)
                body.Push(test.body[i]);
            bool continues = false;
            return !Touches(m, f, body, out.local, 0, continues) && !continues;
        }

        // The values the counter takes, or false when there are more than `max`.
        bool Trips(const Counted& c, u32 max, Array<i64>& values)
        {
            values.Clear();
            for (i64 i = c.start;; i += c.step)
            {
                bool run = c.op == Op::Lt ? i < c.limit : c.op == Op::Le ? i <= c.limit : c.op == Op::Gt ? i > c.limit : c.op == Op::Ge ? i >= c.limit : i != c.limit;
                if (!run)
                    return true;
                if (values.Size() >= max)
                    return false;
                values.Push(i);
            }
        }

        ExprId Clone(Function& f, ExprId id, u32 local, i64 value)
        {
            if (!id)
                return 0;
            Expr e = f.exprs[id];
            if (e.kind == ExprKind::Local && e.index == local)
            {
                Type type = e.type;
                e = Expr();
                e.kind = ExprKind::ConstInt;
                e.type = type;
                e.integer = type == kUInt ? i64(u32(value)) : value;
                return AddExpr(f, Move(e));
            }
            e.a = Clone(f, e.a, local, value);
            e.b = Clone(f, e.b, local, value);
            for (ExprId& arg : e.args)
                arg = Clone(f, arg, local, value);
            return AddExpr(f, Move(e));
        }

        void CloneStatements(Function& f, const Array<Stmt>& from, u32 local, i64 value, Array<Stmt>& out)
        {
            for (const Stmt& s : from)
            {
                Stmt c;
                c.kind = s.kind;
                c.a = Clone(f, s.a, local, value);
                c.b = Clone(f, s.b, local, value);
                CloneStatements(f, s.body, local, value, c.body);
                CloneStatements(f, s.elseBody, local, value, c.elseBody);
                out.Push(Move(c));
            }
        }

        bool HasBreak(const Array<Stmt>& stmts)
        {
            for (const Stmt& s : stmts)
            {
                if (s.kind == StmtKind::Break)
                    return true;
                // Breaks of nested loops are theirs.
                if (s.kind != StmtKind::Loop && (HasBreak(s.body) || HasBreak(s.elseBody)))
                    return true;
            }
            return false;
        }

        bool UnrollIn(const Module& m, Function& f, Array<Stmt>& stmts)
        {
            bool changed = false;
            for (Stmt& s : stmts)
            {
                changed = UnrollIn(m, f, s.body) || changed;
                changed = UnrollIn(m, f, s.elseBody) || changed;
            }
            for (usize i = 0; i + 1 < stmts.Size(); i++)
            {
                Counted c;
                Array<Stmt> body;
                if (!Recognize(m, f, stmts[i], stmts[i + 1], c, body))
                    continue;
                Array<i64> values;
                bool asked = f.loops == LoopHint::Unroll;
                if (!Trips(c, asked ? kMaxTrips : kAutoTrips, values))
                    continue;
                if (!asked && CountStatements(body) * values.Size() > kAutoStatements)
                    continue;
                // One copy of the body per value of the counter, each after setting the counter (so it has the value of the
                // loop after a break); a break leaves a loop that runs once around them.
                auto setCounter = [&](i64 value) {
                    Expr place, constant;
                    place.kind = ExprKind::Local;
                    place.index = c.local;
                    place.type = f.locals[c.local].type;
                    constant.kind = ExprKind::ConstInt;
                    constant.type = place.type;
                    constant.integer = place.type == kUInt ? i64(u32(value)) : value;
                    Stmt assign;
                    assign.kind = StmtKind::Assign;
                    assign.a = AddExpr(f, Move(place));
                    assign.b = AddExpr(f, Move(constant));
                    return assign;
                };
                Array<Stmt> unrolled;
                for (i64 v : values)
                {
                    unrolled.Push(setCounter(v));
                    CloneStatements(f, body, c.local, v, unrolled);
                }
                Stmt counter = setCounter(values.Empty() ? c.start : values[values.Size() - 1] + c.step);
                Stmt replacement;
                if (HasBreak(body))
                {
                    replacement.kind = StmtKind::Loop;
                    replacement.body = Move(unrolled);
                    replacement.body.Push(Move(counter));
                    Stmt leave;
                    leave.kind = StmtKind::Break;
                    replacement.body.Push(Move(leave));
                    stmts[i + 1] = Move(replacement);
                }
                else
                {
                    stmts.RemoveAt(i + 1);
                    unrolled.Push(Move(counter));
                    for (usize k = 0; k < unrolled.Size(); k++)
                        stmts.Insert(i + 1 + k, Move(unrolled[k]));
                }
                changed = true;
            }
            return changed;
        }
    }

    bool UnrollLoops(Module& module, String&)
    {
        for (Function& f : module.functions)
            // A nested loop whose limit was the outer counter has a constant limit after the outer one is unrolled.
            for (u32 round = 0; f.loops != LoopHint::Keep && round < 4 && UnrollIn(module, f, f.body); round++)
            {
            }
        return true;
    }
}
