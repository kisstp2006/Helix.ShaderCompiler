#include <Helix/ShaderCompiler/Frontend/Implementation/Flow.h>

namespace helix::shadercompiler
{
    ExprId MakeNot(Function& function, ExprId condition)
    {
        // Comparisons flip instead of nesting, a double negation cancels.
        Expr e = function.exprs[condition];
        if (e.kind == ExprKind::Unary && e.op == Op::Not)
            return e.a;
        if (e.kind == ExprKind::ConstBool)
        {
            e.integer = !e.integer;
            return AddExpr(function, Move(e));
        }
        bool integers = e.kind == ExprKind::Binary && function.exprs[e.a].type.base != BaseType::Float;
        if (e.kind == ExprKind::Binary && (e.op == Op::Eq || e.op == Op::Ne || (integers && e.op >= Op::Lt)))
        {
            // Ordered float comparisons are not flipped: !(a < b) differs from a >= b for NaN.
            static constexpr Op kFlip[] = {Op::Ne, Op::Eq, Op::Ge, Op::Gt, Op::Le, Op::Lt};
            e.op = kFlip[u32(e.op) - u32(Op::Eq)];
            return AddExpr(function, Move(e));
        }
        return MakeUnary(function, Op::Not, condition, kBool);
    }

    namespace
    {
        struct Structurer
        {
            Array<FlowBlock>& blocks;
            Function& fn;
            String& error;

            Structurer(Array<FlowBlock>& b, Function& f, String& e) : blocks(b), fn(f), error(e) {}

            usize count = 0;
            Array<i32> order;      // reverse postorder
            Array<i32> rank;       // position in order, -1 when unreachable
            Array<i32> idom;
            Array<u32> forwardPreds;
            Array<u8> isHeader;
            Array<i32> loopFollow; // per header, -1 when the loop is only left by returns
            Array<Array<u8>> loopBody;
            Array<u32> emitted;

            struct Context
            {
                i32 follow = -1;
                i32 loopHeader = -1;
                i32 loopFollow = -1;
            };

            bool Fail(const char* message)
            {
                if (error.Empty())
                    error = message;
                return false;
            }

            bool Terminal(i32 b) const { return blocks[usize(b)].end == FlowBlock::End::Return; }

            u32 SuccessorCount(i32 b) const
            {
                FlowBlock::End end = blocks[usize(b)].end;
                return end == FlowBlock::End::Return ? 0 : end == FlowBlock::End::Goto ? 1 : 2;
            }

            void ComputeOrder()
            {
                order.Clear();
                rank.Clear();
                rank.Resize(count, -1);
                Array<u8> state;
                state.Resize(count, u8(0));
                struct Frame
                {
                    i32 block;
                    u32 next;
                };
                Array<Frame> stack;
                Array<i32> post;
                stack.Push({0, 0});
                state[0] = 1;
                while (!stack.Empty())
                {
                    Frame& f = stack.Back();
                    if (f.next < SuccessorCount(f.block))
                    {
                        i32 s = blocks[usize(f.block)].succ[f.next++];
                        if (!state[usize(s)])
                        {
                            state[usize(s)] = 1;
                            stack.Push({s, 0});
                        }
                    }
                    else
                    {
                        post.Push(f.block);
                        stack.Pop();
                    }
                }
                for (usize i = post.Size(); i-- > 0;)
                {
                    rank[usize(post[i])] = i32(order.Size());
                    order.Push(post[i]);
                }
            }

            // Removes empty blocks that only jump on, and branches whose two targets are the same.
            void Simplify()
            {
                for (bool changed = true; changed;)
                {
                    changed = false;
                    for (usize b = 0; b < count; b++)
                    {
                        FlowBlock& block = blocks[b];
                        if (block.end == FlowBlock::End::Branch && block.succ[0] == block.succ[1])
                        {
                            block.end = FlowBlock::End::Goto;
                            changed = true;
                        }
                        for (u32 k = 0; k < SuccessorCount(i32(b)); k++)
                        {
                            const FlowBlock& target = blocks[usize(block.succ[k])];
                            if (target.stmts.Empty() && target.end == FlowBlock::End::Goto && target.succ[0] != block.succ[k] && block.succ[k] != 0)
                            {
                                block.succ[k] = target.succ[0];
                                changed = true;
                            }
                        }
                    }
                }
            }

            u32 CountPreds(i32 target) const
            {
                u32 n = 0;
                for (usize b = 0; b < count; b++)
                    if (rank[b] >= 0)
                        for (u32 k = 0; k < SuccessorCount(i32(b)); k++)
                            n += blocks[b].succ[k] == target ? 1 : 0;
                return n;
            }

            // a && b and a || b arrive as two branch blocks that share a target; fold them into one condition.
            void MergeShortCircuits()
            {
                for (bool changed = true; changed;)
                {
                    changed = false;
                    ComputeOrder();
                    for (usize ai = 0; ai < count && !changed; ai++)
                    {
                        FlowBlock& a = blocks[ai];
                        if (rank[ai] < 0 || a.end != FlowBlock::End::Branch)
                            continue;
                        for (u32 side = 0; side < 2 && !changed; side++)
                        {
                            i32 bi = a.succ[side], other = a.succ[1 - side];
                            const FlowBlock& b = blocks[usize(bi)];
                            if (bi == i32(ai) || b.end != FlowBlock::End::Branch || !b.stmts.Empty() || CountPreds(bi) != 1 || bi == 0)
                                continue;
                            if (b.succ[0] != other && b.succ[1] != other)
                                continue;
                            ExprId ca = a.value, cb = b.value;
                            i32 bTrue = b.succ[0], bFalse = b.succ[1];
                            if (side == 1)
                            {
                                // a false leads to b: taken when a || b (or a || !b).
                                if (bTrue == other)
                                {
                                    a.value = MakeBinary(fn, Op::LogicalOr, ca, cb, kBool);
                                    a.succ[1] = bFalse;
                                }
                                else
                                {
                                    a.value = MakeBinary(fn, Op::LogicalOr, ca, MakeNot(fn, cb), kBool);
                                    a.succ[1] = bTrue;
                                }
                            }
                            else
                            {
                                // a true leads to b: taken when a && b (or a && !b).
                                if (bFalse == other)
                                {
                                    a.value = MakeBinary(fn, Op::LogicalAnd, ca, cb, kBool);
                                    a.succ[0] = bTrue;
                                }
                                else
                                {
                                    a.value = MakeBinary(fn, Op::LogicalAnd, ca, MakeNot(fn, cb), kBool);
                                    a.succ[0] = bFalse;
                                }
                            }
                            changed = true;
                        }
                    }
                    if (changed)
                        Simplify();
                }
            }

            i32 Intersect(i32 a, i32 b) const
            {
                while (a != b)
                {
                    while (rank[usize(a)] > rank[usize(b)])
                        a = idom[usize(a)];
                    while (rank[usize(b)] > rank[usize(a)])
                        b = idom[usize(b)];
                }
                return a;
            }

            bool Dominates(i32 a, i32 b) const
            {
                while (b != a && b != 0)
                    b = idom[usize(b)];
                return b == a;
            }

            bool Analyze()
            {
                ComputeOrder();
                idom.Clear();
                idom.Resize(count, -1);
                idom[0] = 0;
                for (bool changed = true; changed;)
                {
                    changed = false;
                    for (usize i = 1; i < order.Size(); i++)
                    {
                        i32 b = order[i], best = -1;
                        for (i32 p : order)
                        {
                            if (idom[usize(p)] < 0)
                                continue;
                            for (u32 k = 0; k < SuccessorCount(p); k++)
                                if (blocks[usize(p)].succ[k] == b)
                                    best = best < 0 ? p : Intersect(p, best);
                        }
                        if (best != idom[usize(b)])
                        {
                            idom[usize(b)] = best;
                            changed = true;
                        }
                    }
                }
                forwardPreds.Clear();
                forwardPreds.Resize(count, 0u);
                isHeader.Clear();
                isHeader.Resize(count, u8(0));
                loopBody.Clear();
                loopBody.Resize(count);
                loopFollow.Clear();
                loopFollow.Resize(count, -1);
                for (i32 u : order)
                {
                    for (u32 k = 0; k < SuccessorCount(u); k++)
                    {
                        i32 v = blocks[usize(u)].succ[k];
                        if (rank[usize(v)] > rank[usize(u)])
                        {
                            forwardPreds[usize(v)]++;
                            continue;
                        }
                        if (!Dominates(v, u))
                            return Fail("irreducible control flow (a jump into the middle of a loop)");
                        // Back edge u -> v: everything that reaches u without passing v is in the loop.
                        isHeader[usize(v)] = 1;
                        Array<u8>& body = loopBody[usize(v)];
                        if (body.Empty())
                            body.Resize(count, u8(0));
                        body[usize(v)] = 1;
                        Array<i32> work;
                        if (!body[usize(u)])
                        {
                            body[usize(u)] = 1;
                            work.Push(u);
                        }
                        while (!work.Empty())
                        {
                            i32 x = work.Back();
                            work.Pop();
                            for (i32 p : order)
                                for (u32 j = 0; j < SuccessorCount(p); j++)
                                    if (blocks[usize(p)].succ[j] == x && !body[usize(p)])
                                    {
                                        body[usize(p)] = 1;
                                        work.Push(p);
                                    }
                        }
                    }
                }
                for (i32 h : order)
                {
                    if (!isHeader[usize(h)])
                        continue;
                    const Array<u8>& body = loopBody[usize(h)];
                    Array<i32> exits;
                    for (i32 u : order)
                        if (body[usize(u)])
                            for (u32 k = 0; k < SuccessorCount(u); k++)
                            {
                                i32 v = blocks[usize(u)].succ[k];
                                if (!body[usize(v)] && !exits.Find(v))
                                    exits.Push(v);
                            }
                    i32 follow = -1;
                    u32 open = 0;
                    for (i32 v : exits)
                        if (!Terminal(v))
                        {
                            follow = v;
                            open++;
                        }
                    if (open > 1)
                        return Fail("a loop is left towards more than one place (goto out of a loop)");
                    if (open == 0 && exits.Size() == 1)
                        follow = exits[0];
                    loopFollow[usize(h)] = follow;
                }
                emitted.Clear();
                emitted.Resize(count, 0u);
                return true;
            }

            // The block where both sides of a branch meet again, -1 when they never do.
            bool IfFollow(i32 n, const Context& ctx, i32& follow)
            {
                follow = -1;
                i32 terminal = -1;
                u32 open = 0, closed = 0;
                for (i32 c : order)
                {
                    if (c == n || idom[usize(c)] != n || forwardPreds[usize(c)] < 2 || c == ctx.loopHeader || c == ctx.loopFollow)
                        continue;
                    if (Terminal(c))
                    {
                        terminal = c;
                        closed++;
                    }
                    else
                    {
                        follow = c;
                        open++;
                    }
                }
                if (open > 1)
                    return Fail("control flow joins in more than one place (goto into shared code)");
                if (open == 0 && closed == 1)
                    follow = terminal;
                return true;
            }

            bool EmitTarget(i32 s, const Context& ctx, Array<Stmt>& out, bool force)
            {
                if (s == ctx.follow)
                    return true;
                if (s == ctx.loopHeader)
                {
                    out.Emplace().kind = StmtKind::Continue;
                    return true;
                }
                if (s == ctx.loopFollow)
                {
                    out.Emplace().kind = StmtKind::Break;
                    return true;
                }
                if (!force && forwardPreds[usize(s)] != 1 && !Terminal(s))
                    return Fail("a jump has no structured form (goto, or a break out of nested loops)");
                return EmitNode(s, ctx, out);
            }

            bool EmitNode(i32 n, const Context& ctx, Array<Stmt>& out)
            {
                if (emitted[usize(n)]++ && !Terminal(n))
                    return Fail("control flow could not be structured (a block would be emitted twice)");
                if (!isHeader[usize(n)])
                    return EmitBody(n, ctx, out);
                Stmt loop;
                loop.kind = StmtKind::Loop;
                Context inner;
                inner.loopHeader = n;
                inner.loopFollow = loopFollow[usize(n)];
                if (!EmitBody(n, inner, loop.body))
                    return false;
                out.Push(Move(loop));
                return inner.loopFollow < 0 || EmitTarget(inner.loopFollow, ctx, out, true);
            }

            bool EmitBody(i32 n, const Context& ctx, Array<Stmt>& out)
            {
                const FlowBlock& block = blocks[usize(n)];
                for (const Stmt& s : block.stmts)
                    out.Push(s);
                if (block.end == FlowBlock::End::Return)
                {
                    Stmt& s = out.Emplace();
                    s.kind = StmtKind::Return;
                    s.a = block.value;
                    return true;
                }
                if (block.end == FlowBlock::End::Goto)
                    return EmitTarget(block.succ[0], ctx, out, false);
                i32 follow;
                if (!IfFollow(n, ctx, follow))
                    return false;
                Context arm = ctx;
                if (follow >= 0)
                    arm.follow = follow;
                Stmt branch;
                branch.kind = StmtKind::If;
                branch.a = block.value;
                i32 yes = block.succ[0], no = block.succ[1];
                if (!EmitTarget(yes, arm, branch.body, false) || !EmitTarget(no, arm, branch.elseBody, false))
                    return false;
                if (branch.body.Empty() && !branch.elseBody.Empty())
                {
                    branch.a = MakeNot(fn, branch.a);
                    Swap(branch.body, branch.elseBody);
                }
                out.Push(Move(branch));
                return follow < 0 || EmitTarget(follow, ctx, out, true);
            }

            bool Run(Array<Stmt>& out)
            {
                count = blocks.Size();
                ComputeOrder();
                Simplify();
                MergeShortCircuits();
                if (!Analyze())
                    return false;
                return EmitNode(0, Context(), out);
            }
        };
    }

    bool StructureFlow(Array<FlowBlock>& blocks, Function& function, Array<Stmt>& out, String& error)
    {
        Structurer s(blocks, function, error);
        return s.Run(out);
    }
}
