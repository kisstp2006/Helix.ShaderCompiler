#include <Helix/ShaderCompiler/Transforms/Implementation/Optimize.h>

#include <Helix/Core/Strings/Format.h>

namespace helix::shadercompiler::passes
{
    namespace
    {
        bool IsZero(const Function& f, ExprId id)
        {
            const Expr& e = f.exprs[id];
            return e.kind == ExprKind::Construct && e.args.Empty();
        }

        // Marks struct locals used as a whole: anything but the base of a field access, or a whole assignment that is not
        // the zero value.
        void Uses(const Function& f, ExprId id, bool fieldBase, Array<u8>& whole, Array<u8>& fields)
        {
            if (!id)
                return;
            const Expr& e = f.exprs[id];
            if (e.kind == ExprKind::Local)
                (fieldBase ? fields : whole)[e.index] = 1;
            Uses(f, e.a, e.kind == ExprKind::Field, whole, fields);
            Uses(f, e.b, false, whole, fields);
            for (ExprId arg : e.args)
                Uses(f, arg, false, whole, fields);
        }

        bool Split(const Module& m, Function& f)
        {
            // A local whose fields the code uses, and only them, comes apart.
            Array<u8> whole, fields;
            whole.Resize(f.locals.Size(), u8(0));
            fields.Resize(f.locals.Size(), u8(0));
            ForEachStatement(f.body, [&](const Stmt& s) {
                // A whole assignment of the zero value splits into one per field.
                if (s.kind == StmtKind::Assign && f.exprs[s.a].kind == ExprKind::Local && IsZero(f, s.b))
                    return;
                Uses(f, s.a, false, whole, fields);
                Uses(f, s.b, false, whole, fields);
            });
            Array<i32> first;
            first.Resize(f.locals.Size(), i32(-1));
            bool any = false;
            usize count = f.locals.Size();
            for (usize l = 0; l < count; l++)
            {
                Type t = f.locals[l].type;
                if (whole[l] || !fields[l] || t.base != BaseType::Struct || m.structs[t.structIndex].fields.Empty())
                    continue;
                first[l] = i32(f.locals.Size());
                for (const StructField& field : m.structs[t.structIndex].fields)
                {
                    Variable v;
                    v.name = Format("{}_{}", f.locals[l].name, field.name);
                    v.type = field.type;
                    f.locals.Push(Move(v));
                }
                any = true;
            }
            if (!any)
                return false;
            // Fields of a split local are locals of their own now.
            for (Expr& e : f.exprs)
                if (e.kind == ExprKind::Field && e.a && f.exprs[e.a].kind == ExprKind::Local && f.exprs[e.a].index < count &&
                    first[f.exprs[e.a].index] >= 0)
                {
                    u32 local = u32(first[f.exprs[e.a].index]) + e.index;
                    Type type = e.type;
                    e = Expr();
                    e.kind = ExprKind::Local;
                    e.index = local;
                    e.type = type;
                }
            auto rewrite = [&](auto&& self, Array<Stmt>& stmts) -> void {
                Array<Stmt> out;
                for (Stmt& s : stmts)
                {
                    self(self, s.body);
                    self(self, s.elseBody);
                    const Expr& target = f.exprs[s.a];
                    if (s.kind == StmtKind::Assign && target.kind == ExprKind::Local && target.index < count && first[target.index] >= 0)
                    {
                        // The zero value, field by field.
                        u32 base = u32(first[target.index]);
                        usize fields = m.structs[f.locals[target.index].type.structIndex].fields.Size();
                        for (u32 i = 0; i < fields; i++)
                        {
                            Expr place, zero;
                            place.kind = ExprKind::Local;
                            place.index = base + i;
                            place.type = f.locals[base + i].type;
                            zero.kind = ExprKind::Construct;
                            zero.type = place.type;
                            Stmt assign;
                            assign.kind = StmtKind::Assign;
                            assign.a = AddExpr(f, Move(place));
                            assign.b = AddExpr(f, Move(zero));
                            out.Push(Move(assign));
                        }
                        continue;
                    }
                    out.Push(Move(s));
                }
                stmts = Move(out);
            };
            rewrite(rewrite, f.body);
            return true;
        }
    }

    bool ReplaceStructLocals(Module& module, String&)
    {
        // Nested structs come apart one level per round.
        for (Function& f : module.functions)
            while (Split(module, f))
            {
            }
        return true;
    }
}
