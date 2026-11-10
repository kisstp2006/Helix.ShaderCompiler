#include <Helix/ShaderCompiler/Transforms/Simplify.h>

#include <cmath>

namespace helix::shadercompiler
{
    namespace
    {
        bool IsConst(const Expr& e) { return e.kind == ExprKind::ConstFloat || e.kind == ExprKind::ConstInt || e.kind == ExprKind::ConstBool; }

        void SetConst(Expr& e, Type type, i64 integer, f64 number)
        {
            e.kind = type.base == BaseType::Float ? ExprKind::ConstFloat : type.base == BaseType::Bool ? ExprKind::ConstBool : ExprKind::ConstInt;
            e.type = type;
            e.integer = type.base == BaseType::Bool ? (integer != 0) : type.base == BaseType::UInt ? i64(u32(integer)) : i64(i32(integer));
            e.number = f64(f32(number));
            e.a = e.b = 0;
            e.args.Clear();
        }

        bool HasCall(const Function& fn, ExprId id)
        {
            if (!id)
                return false;
            const Expr& e = fn.exprs[id];
            // An atomic add changes a buffer like a call may.
            if (e.kind == ExprKind::Call || (e.kind == ExprKind::Intrinsic && e.intrinsic == Intrinsic::InterlockedAdd))
                return true;
            for (ExprId arg : e.args)
                if (HasCall(fn, arg))
                    return true;
            return HasCall(fn, e.a) || HasCall(fn, e.b);
        }

        bool FoldBinary(Expr& e, const Expr& x, const Expr& y)
        {
            Type t = x.type;
            bool truth = false, compare = true;
            if (t.base == BaseType::Float)
            {
                f32 a = f32(x.number), b = f32(y.number), r = 0;
                switch (e.op)
                {
                case Op::Eq: truth = a == b; break;
                case Op::Ne: truth = a != b; break;
                case Op::Lt: truth = a < b; break;
                case Op::Le: truth = a <= b; break;
                case Op::Gt: truth = a > b; break;
                case Op::Ge: truth = a >= b; break;
                case Op::Add: r = a + b; compare = false; break;
                case Op::Sub: r = a - b; compare = false; break;
                case Op::Mul: r = a * b; compare = false; break;
                default: return false;
                }
                SetConst(e, compare ? kBool : kFloat, truth, r);
                return true;
            }
            bool isUnsigned = t.base == BaseType::UInt;
            i64 a = x.integer, b = y.integer, r = 0;
            u32 ua = u32(a), ub = u32(b);
            switch (e.op)
            {
            case Op::Eq: truth = a == b; break;
            case Op::Ne: truth = a != b; break;
            case Op::Lt: truth = a < b; break;
            case Op::Le: truth = a <= b; break;
            case Op::Gt: truth = a > b; break;
            case Op::Ge: truth = a >= b; break;
            case Op::LogicalAnd: truth = a && b; break;
            case Op::LogicalOr: truth = a || b; break;
            case Op::Add: r = i64(ua + ub); compare = false; break;
            case Op::Sub: r = i64(ua - ub); compare = false; break;
            case Op::Mul: r = i64(ua * ub); compare = false; break;
            case Op::And: r = i64(ua & ub); compare = false; break;
            case Op::Or: r = i64(ua | ub); compare = false; break;
            case Op::Xor: r = i64(ua ^ ub); compare = false; break;
            case Op::Shl: r = i64(ua << (ub & 31)); compare = false; break;
            case Op::Shr: r = isUnsigned ? i64(ua >> (ub & 31)) : i64(i32(ua) >> (ub & 31)); compare = false; break;
            default: return false;
            }
            SetConst(e, compare ? kBool : e.type, compare ? i64(truth) : r, 0);
            return true;
        }

        // Folds the expression tree; returns the id that now stands for it.
        ExprId Fold(Function& fn, ExprId id)
        {
            if (!id)
                return 0;
            {
                Expr& e = fn.exprs[id];
                e.a = Fold(fn, e.a);
                e.b = Fold(fn, e.b);
                for (ExprId& arg : e.args)
                    arg = Fold(fn, arg);
            }
            Expr& e = fn.exprs[id];
            auto scalar = [&](ExprId x) { return x && IsConst(fn.exprs[x]) && fn.exprs[x].type.IsScalar(); };
            if (e.kind == ExprKind::Unary && scalar(e.a))
            {
                Expr x = fn.exprs[e.a];
                if (e.op == Op::Not)
                    SetConst(e, kBool, x.integer == 0, 0);
                else if (e.op == Op::Neg)
                    SetConst(e, x.type, i64(0u - u32(x.integer)), -x.number);
                else if (e.op == Op::BitNot)
                    SetConst(e, x.type, i64(u32(x.integer) ^ 0xFFFFFFFFu), 0);
            }
            else if (e.kind == ExprKind::Cast && scalar(e.a) && e.type.IsScalar())
            {
                Expr x = fn.exprs[e.a];
                bool fromFloat = x.type.base == BaseType::Float;
                if (e.type.base == BaseType::Float)
                    SetConst(e, kFloat, 0, fromFloat ? x.number : x.type.base == BaseType::UInt ? f64(u32(x.integer)) : f64(x.integer));
                else if (e.type.base == BaseType::Bool)
                    SetConst(e, kBool, fromFloat ? x.number != 0 : x.integer != 0, 0);
                else if (!fromFloat)
                    SetConst(e, e.type, x.integer, 0);
                else if (std::fabs(x.number) < 2147483648.0)
                    SetConst(e, e.type, i64(x.number), 0);
            }
            else if (e.kind == ExprKind::Binary)
            {
                bool ca = scalar(e.a), cb = scalar(e.b);
                if (ca && cb)
                {
                    Expr x = fn.exprs[e.a], y = fn.exprs[e.b];
                    FoldBinary(e, x, y);
                }
                else if ((e.op == Op::LogicalAnd || e.op == Op::LogicalOr) && (ca || cb))
                {
                    // One side decides the result or leaves it to the other side.
                    bool value = fn.exprs[ca ? e.a : e.b].integer != 0;
                    ExprId other = ca ? e.b : e.a;
                    bool decides = value == (e.op == Op::LogicalOr);
                    if (!decides)
                        return other;
                    if (ca || !HasCall(fn, other))
                        SetConst(e, kBool, value, 0);
                }
            }
            return id;
        }

        bool Ends(StmtKind kind) { return kind == StmtKind::Break || kind == StmtKind::Continue || kind == StmtKind::Return; }

        void Statements(Function& fn, Array<Stmt>& stmts)
        {
            Array<Stmt> out;
            auto add = [&](Stmt&& s) {
                bool ends = Ends(s.kind);
                out.Push(Move(s));
                return !ends;
            };
            bool open = true;
            for (usize i = 0; i < stmts.Size() && open; i++)
            {
                Stmt& s = stmts[i];
                s.a = Fold(fn, s.a);
                s.b = Fold(fn, s.b);
                Statements(fn, s.body);
                Statements(fn, s.elseBody);
                if (s.kind == StmtKind::If)
                {
                    const Expr& condition = fn.exprs[s.a];
                    if (IsConst(condition))
                    {
                        for (Stmt& taken : condition.integer ? s.body : s.elseBody)
                            if (open)
                                open = add(Move(taken));
                        continue;
                    }
                    if (s.body.Empty() && s.elseBody.Empty() && !HasCall(fn, s.a))
                        continue;
                }
                // A loop that leaves at once does nothing.
                if (s.kind == StmtKind::Loop && !s.body.Empty() && s.body[0].kind == StmtKind::Break)
                    continue;
                open = add(Move(s));
            }
            stmts = Move(out);
        }

        // A local written once, with a constant, has that value wherever it is read: the C# compiler only lets code read
        // a local after its assignment. This is how a keyword that went through a local (as in a switch) still folds.
        struct Writes
        {
            Array<u8> count;
            Array<ExprId> value, target;
        };

        i32 RootLocal(const Function& fn, ExprId id, bool& whole)
        {
            whole = true;
            while (id)
            {
                const Expr& e = fn.exprs[id];
                if (e.kind == ExprKind::Local)
                    return i32(e.index);
                if (e.kind != ExprKind::Field && e.kind != ExprKind::Swizzle && e.kind != ExprKind::Index)
                    break;
                whole = false;
                id = e.a;
            }
            return -1;
        }

        void CountCalls(const Module& m, const Function& fn, ExprId id, Writes& w)
        {
            if (!id)
                return;
            const Expr& e = fn.exprs[id];
            if (e.kind == ExprKind::Call)
            {
                const Function& callee = m.functions[e.index];
                for (usize i = 0; i < e.args.Size() && i < callee.params.Size(); i++)
                {
                    bool whole;
                    i32 local = callee.params[i].mode != ParamMode::In ? RootLocal(fn, e.args[i], whole) : -1;
                    if (local >= 0)
                        w.count[usize(local)] = 2;
                }
            }
            CountCalls(m, fn, e.a, w);
            CountCalls(m, fn, e.b, w);
            for (ExprId arg : e.args)
                CountCalls(m, fn, arg, w);
        }

        void CountWrites(const Module& m, const Function& fn, const Array<Stmt>& stmts, Writes& w)
        {
            for (const Stmt& s : stmts)
            {
                CountCalls(m, fn, s.a, w);
                CountCalls(m, fn, s.b, w);
                if (s.kind == StmtKind::Assign)
                {
                    bool whole;
                    i32 local = RootLocal(fn, s.a, whole);
                    if (local >= 0)
                    {
                        u8& count = w.count[usize(local)];
                        bool constant = whole && IsConst(fn.exprs[s.b]) && fn.exprs[s.b].type == fn.locals[usize(local)].type;
                        count = u8(count || !constant ? 2 : 1);
                        w.value[usize(local)] = s.b;
                        w.target[usize(local)] = s.a;
                    }
                }
                CountWrites(m, fn, s.body, w);
                CountWrites(m, fn, s.elseBody, w);
            }
        }

        void DropAssignments(Array<Stmt>& stmts, const Writes& w, const Function& fn)
        {
            Array<Stmt> out;
            for (Stmt& s : stmts)
            {
                DropAssignments(s.body, w, fn);
                DropAssignments(s.elseBody, w, fn);
                bool whole;
                i32 local = s.kind == StmtKind::Assign ? RootLocal(fn, s.a, whole) : -1;
                if (local < 0 || w.count[usize(local)] != 1)
                    out.Push(Move(s));
            }
            stmts = Move(out);
        }

        bool Propagate(const Module& m, Function& fn)
        {
            Writes w;
            w.count.Resize(fn.locals.Size());
            w.value.Resize(fn.locals.Size());
            w.target.Resize(fn.locals.Size());
            for (u8& c : w.count)
                c = 0;
            CountWrites(m, fn, fn.body, w);
            bool any = false;
            for (u8 c : w.count)
                any = any || c == 1;
            if (!any)
                return false;
            DropAssignments(fn.body, w, fn);
            for (usize id = 1; id < fn.exprs.Size(); id++)
            {
                Expr& e = fn.exprs[id];
                if (e.kind != ExprKind::Local || w.count[e.index] != 1)
                    continue;
                Expr value = fn.exprs[w.value[e.index]];
                SetConst(e, value.type, value.integer, value.number);
            }
            return true;
        }

        // Marks what the statements that are left still refer to.
        void Mark(const Function& fn, ExprId id, Array<u8>& functions, Array<u8>& globals)
        {
            if (!id)
                return;
            const Expr& e = fn.exprs[id];
            if (e.kind == ExprKind::Call)
                functions[e.index] = 1;
            else if (e.kind == ExprKind::Global)
                globals[e.index] = 1;
            Mark(fn, e.a, functions, globals);
            Mark(fn, e.b, functions, globals);
            for (ExprId arg : e.args)
                Mark(fn, arg, functions, globals);
        }

        void Mark(const Function& fn, const Array<Stmt>& stmts, Array<u8>& functions, Array<u8>& globals)
        {
            for (const Stmt& s : stmts)
            {
                Mark(fn, s.a, functions, globals);
                Mark(fn, s.b, functions, globals);
                Mark(fn, s.body, functions, globals);
                Mark(fn, s.elseBody, functions, globals);
            }
        }
    }

    void Simplify(Module& m)
    {
        for (Function& fn : m.functions)
        {
            Statements(fn, fn.body);
            while (Propagate(m, fn))
                Statements(fn, fn.body);
        }

        Array<u8> functions, globals;
        functions.Resize(m.functions.Size());
        globals.Resize(m.globals.Size());
        for (u8& f : functions)
            f = 0;
        for (u8& g : globals)
            g = 0;
        functions[m.entry] = 1;
        // Callees come before their callers, so one pass from the end reaches everything.
        for (usize i = m.functions.Size(); i-- > 0;)
            if (functions[i])
                Mark(m.functions[i], m.functions[i].body, functions, globals);

        constexpr u32 kGone = ~0u;
        Array<u32> functionMap, globalMap;
        Array<Function> keptFunctions;
        Array<Global> keptGlobals;
        for (usize i = 0; i < m.functions.Size(); i++)
        {
            functionMap.Push(functions[i] ? u32(keptFunctions.Size()) : kGone);
            if (functions[i])
                keptFunctions.Push(Move(m.functions[i]));
        }
        for (usize i = 0; i < m.globals.Size(); i++)
        {
            globalMap.Push(globals[i] ? u32(keptGlobals.Size()) : kGone);
            if (globals[i])
                keptGlobals.Push(Move(m.globals[i]));
        }
        m.entry = functionMap[m.entry];
        m.functions = Move(keptFunctions);
        m.globals = Move(keptGlobals);
        for (Function& fn : m.functions)
            for (Expr& e : fn.exprs)
            {
                if (e.kind != ExprKind::Call && e.kind != ExprKind::Global)
                    continue;
                u32 mapped = e.kind == ExprKind::Call ? functionMap[e.index] : globalMap[e.index];
                // Expressions of removed statements stay in the table; they must not point at what is gone.
                if (mapped == kGone)
                    SetConst(e, kInt, 0, 0);
                else
                    e.index = mapped;
            }
    }
}
