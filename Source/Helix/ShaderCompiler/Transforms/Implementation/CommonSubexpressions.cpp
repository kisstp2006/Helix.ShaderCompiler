#include <Helix/ShaderCompiler/Transforms/Implementation/Optimize.h>

#include <Helix/Core/Strings/Format.h>

#include <cstring>

namespace helix::shadercompiler::passes
{
    namespace
    {
        constexpr u32 kUnowned = ~0u;

        // Gives every statement its own expression tree: a node another statement already uses is copied, so a node can
        // be rewritten for one statement without changing another.
        struct Owner
        {
            Function& f;
            Array<u32> owner;
            u32 statement = 0;

            explicit Owner(Function& function) : f(function) { owner.Resize(f.exprs.Size(), kUnowned); }

            ExprId Own(ExprId id)
            {
                if (!id)
                    return 0;
                if (owner[id] == statement)
                    return id;
                if (owner[id] != kUnowned)
                {
                    Expr copy = f.exprs[id];
                    id = AddExpr(f, Move(copy));
                    owner.Push(kUnowned);
                }
                owner[id] = statement;
                ExprId a = Own(f.exprs[id].a), b = Own(f.exprs[id].b);
                f.exprs[id].a = a;
                f.exprs[id].b = b;
                for (usize i = 0; i < f.exprs[id].args.Size(); i++)
                {
                    ExprId arg = Own(f.exprs[id].args[i]);
                    f.exprs[id].args[i] = arg;
                }
                return id;
            }

            void Statements(Array<Stmt>& stmts)
            {
                for (Stmt& s : stmts)
                {
                    statement++;
                    s.a = Own(s.a);
                    s.b = Own(s.b);
                    Statements(s.body);
                    Statements(s.elseBody);
                }
            }
        };

        struct Candidate
        {
            // Hashed again on every comparison: a rewrite below it changes its hash.
            ExprId node;
            // A local holding the value, or -1 while only one statement computed it.
            i32 local;
            // The statement list and the position of the statement that computed it first.
            Array<Stmt>* list;
            usize position;
            // Variables the value reads: kind (Local, Param, Global) and index.
            Array<u64> reads;
        };

        struct Eliminator
        {
            const Module& m;
            Function& f;
            const Array<u8>& pure;
            Array<u64> hashes;
            Array<Candidate> available;
            u32 created = 0;

            Eliminator(const Module& module, Function& function, const Array<u8>& functionsPure) : m(module), f(function), pure(functionsPure) {}

            static u64 Mix(u64 h, u64 v) { return (h ^ v) * 0x100000001B3ull; }

            u64 Hash(ExprId id)
            {
                if (id < hashes.Size() && hashes[id])
                    return hashes[id];
                const Expr& e = f.exprs[id];
                u64 number = 0;
                std::memcpy(&number, &e.number, 8);
                u64 h = 0xCBF29CE484222325ull;
                for (u64 v : {u64(e.kind), u64(e.op), u64(e.intrinsic), u64(e.type.base), u64(e.type.rows), u64(e.type.cols), u64(e.type.structIndex),
                              u64(e.type.half), u64(e.index), u64(e.integer), number, u64(e.swizzleCount)})
                    h = Mix(h, v);
                for (u32 i = 0; i < 4; i++)
                    h = Mix(h, e.swizzle[i]);
                for (ExprId child : {e.a, e.b})
                    h = Mix(h, child ? Hash(child) : 7);
                for (ExprId arg : e.args)
                    h = Mix(h, Hash(arg));
                h = h ? h : 1;
                if (id >= hashes.Size())
                    hashes.Resize(id + 1, u64(0));
                hashes[id] = h;
                return h;
            }

            bool Same(ExprId x, ExprId y)
            {
                if (x == y)
                    return true;
                if (!x || !y || Hash(x) != Hash(y))
                    return false;
                const Expr& p = f.exprs[x];
                const Expr& q = f.exprs[y];
                if (p.kind != q.kind || p.op != q.op || p.intrinsic != q.intrinsic || !(p.type == q.type) || p.index != q.index || p.integer != q.integer ||
                    std::memcmp(&p.number, &q.number, 8) != 0 || p.swizzleCount != q.swizzleCount || std::memcmp(p.swizzle, q.swizzle, 4) != 0 ||
                    p.args.Size() != q.args.Size())
                    return false;
                if (!Same(p.a, q.a) || !Same(p.b, q.b))
                    return false;
                for (usize i = 0; i < p.args.Size(); i++)
                    if (!Same(p.args[i], q.args[i]))
                        return false;
                return true;
            }

            void Reads(ExprId id, Array<u64>& out)
            {
                if (!id)
                    return;
                const Expr& e = f.exprs[id];
                if (e.kind == ExprKind::Local || e.kind == ExprKind::Param || e.kind == ExprKind::Global)
                    out.Push((u64(e.kind) << 32) | e.index);
                Reads(e.a, out);
                Reads(e.b, out);
                for (ExprId arg : e.args)
                    Reads(arg, out);
            }

            // Values worth keeping: operations, not leaves or plain accesses.
            bool Worth(const Expr& e) const
            {
                switch (e.kind)
                {
                case ExprKind::Unary: case ExprKind::Binary: case ExprKind::Cast: case ExprKind::Construct: return !e.args.Empty() || e.kind != ExprKind::Construct;
                case ExprKind::Intrinsic: return e.intrinsic != Intrinsic::InterlockedAdd && e.intrinsic != Intrinsic::BindlessStore;
                default: return false;
                }
            }

            void Forget(u64 variable)
            {
                Array<Candidate> kept;
                for (Candidate& c : available)
                {
                    bool reads = false;
                    for (u64 r : c.reads)
                        reads = reads || r == variable;
                    if (!reads)
                        kept.Push(Move(c));
                }
                available = Move(kept);
            }

            // Visits the operations of a statement bottom up: a value computed before becomes a read of its local.
            void Visit(ExprId id, Array<Stmt>& list, usize position, usize& inserted)
            {
                if (!id)
                    return;
                Visit(f.exprs[id].a, list, position, inserted);
                Visit(f.exprs[id].b, list, position, inserted);
                for (usize i = 0; i < f.exprs[id].args.Size(); i++)
                    Visit(f.exprs[id].args[i], list, position, inserted);
                if (!Worth(f.exprs[id]))
                    return;
                for (Candidate& c : available)
                {
                    if (c.node == id || Hash(c.node) != Hash(id) || !Same(c.node, id))
                        continue;
                    if (c.local < 0)
                    {
                        // The first statement that computed it now computes it into a local, right before itself.
                        Variable v;
                        v.name = Format("c{}", created++);
                        v.type = f.exprs[c.node].type;
                        f.locals.Push(Move(v));
                        c.local = i32(f.locals.Size() - 1);
                        Expr value = f.exprs[c.node];
                        ExprId moved = AddExpr(f, Move(value));
                        Expr read;
                        read.kind = ExprKind::Local;
                        read.index = u32(c.local);
                        read.type = f.exprs[moved].type;
                        f.exprs[c.node] = read;
                        Expr target = read;
                        Stmt assign;
                        assign.kind = StmtKind::Assign;
                        assign.a = AddExpr(f, Move(target));
                        assign.b = moved;
                        c.list->Insert(c.position, Move(assign));
                        // Later statements of the list moved down by one, the current one among them.
                        for (Candidate& other : available)
                            if (&other != &c && other.list == c.list && other.position >= c.position)
                                other.position++;
                        if (c.list == &list && c.position <= position + inserted)
                            inserted++;
                        c.position++;
                        // Later matches compare with the value, which the node no longer holds.
                        c.node = moved;
                        hashes.Clear();
                    }
                    Expr read;
                    read.kind = ExprKind::Local;
                    read.index = u32(c.local);
                    read.type = f.exprs[id].type;
                    f.exprs[id] = read;
                    hashes.Clear();
                    return;
                }
                Candidate c;
                c.node = id;
                c.local = -1;
                c.list = &list;
                c.position = position + inserted;
                Reads(id, c.reads);
                available.Push(Move(c));
            }

            void Statements(Array<Stmt>& stmts)
            {
                // A run starts with nothing known.
                available.Clear();
                for (usize i = 0; i < stmts.Size(); i++)
                {
                    Stmt& s = stmts[i];
                    if (s.kind == StmtKind::If || s.kind == StmtKind::Loop)
                    {
                        // The condition is computed in this run.
                        usize inserted = 0;
                        if (s.kind == StmtKind::If && !HasEffects(m, f, s.a, pure))
                            Visit(s.a, stmts, i, inserted);
                        i += inserted;
                        Stmt& branch = stmts[i];
                        Statements(branch.body);
                        Statements(branch.elseBody);
                        // The branches may have written anything.
                        available.Clear();
                        continue;
                    }
                    // Visiting may insert statements before this one, so its fields are taken first.
                    StmtKind kind = s.kind;
                    ExprId target = s.a, value = s.b;
                    bool effects = HasEffects(m, f, target, pure) || HasEffects(m, f, value, pure);
                    usize inserted = 0;
                    if (!effects && kind != StmtKind::Discard)
                    {
                        Visit(value, stmts, i, inserted);
                        if (kind == StmtKind::Assign)
                        {
                            // Subscripts of the target are values too.
                            for (ExprId place = target; place && f.exprs[place].kind != ExprKind::Local;)
                            {
                                if (f.exprs[place].kind == ExprKind::Index)
                                    Visit(f.exprs[place].b, stmts, i, inserted);
                                place = f.exprs[place].a;
                            }
                        }
                        else
                            Visit(target, stmts, i, inserted);
                    }
                    i += inserted;
                    Stmt& now = stmts[i];
                    if (effects)
                        available.Clear();
                    else if (now.kind == StmtKind::Assign)
                    {
                        ExprId root = now.a;
                        while (root && (f.exprs[root].kind == ExprKind::Field || f.exprs[root].kind == ExprKind::Swizzle || f.exprs[root].kind == ExprKind::Index))
                            root = f.exprs[root].a;
                        Forget((u64(f.exprs[root].kind) << 32) | f.exprs[root].index);
                    }
                }
                available.Clear();
            }
        };
    }

    bool EliminateCommonSubexpressions(Module& module, String&)
    {
        Array<u8> pure = PureFunctions(module);
        for (Function& f : module.functions)
        {
            Owner owner(f);
            owner.Statements(f.body);
            Eliminator e(module, f, pure);
            e.Statements(f.body);
        }
        return true;
    }
}
