#include <Helix/ShaderCompiler/Transforms/Passes.h>

#include <Helix/Core/Strings/Format.h>
#include <Helix/ShaderCompiler/Transforms/Implementation/Optimize.h>
#include <Helix/ShaderCompiler/Transforms/Simplify.h>

namespace helix::shadercompiler
{
    namespace
    {
        // Types match when they hold the same data; half and float differ only in precision.
        bool SameData(Type x, Type y) { return x.base == y.base && x.rows == y.rows && x.cols == y.cols && x.structIndex == y.structIndex; }

        struct Verifier
        {
            const Module& m;
            String& error;
            const Function* f = nullptr;
            usize self = 0;

            bool Fail(StringView message)
            {
                if (error.Empty())
                    error = Format("{}: {}", f ? StringView(f->name) : StringView("module"), message);
                return false;
            }

            bool TypeOk(Type t)
            {
                if (t.base == BaseType::Struct && t.structIndex >= m.structs.Size())
                    return Fail("a type names a struct that does not exist");
                if (t.base == BaseType::Array && t.structIndex >= m.arrays.Size())
                    return Fail("a type names an array that does not exist");
                return true;
            }

            bool Place(ExprId id)
            {
                for (;;)
                {
                    const Expr& e = f->exprs[id];
                    if (e.kind == ExprKind::Local || e.kind == ExprKind::Param || e.kind == ExprKind::Global)
                        return true;
                    if (e.kind != ExprKind::Field && e.kind != ExprKind::Swizzle && e.kind != ExprKind::Index)
                        return false;
                    id = e.a;
                }
            }

            bool Expression(ExprId id)
            {
                if (id == 0 || id >= f->exprs.Size())
                    return Fail(Format("expression {} does not exist", id));
                const Expr& e = f->exprs[id];
                if (!TypeOk(e.type))
                    return false;
                for (ExprId child : {e.a, e.b})
                    if (child && !Expression(child))
                        return false;
                for (ExprId arg : e.args)
                    if (!Expression(arg))
                        return false;
                Type a = e.a ? f->exprs[e.a].type : Type();
                switch (e.kind)
                {
                case ExprKind::Local:
                    if (e.index >= f->locals.Size())
                        return Fail(Format("local {} does not exist", e.index));
                    break;
                case ExprKind::Param:
                    if (e.index >= f->params.Size())
                        return Fail(Format("parameter {} does not exist", e.index));
                    break;
                case ExprKind::Global:
                    if (e.index >= m.globals.Size())
                        return Fail(Format("global {} does not exist", e.index));
                    break;
                case ExprKind::Field:
                    if (!e.a || a.base != BaseType::Struct || a.structIndex >= m.structs.Size() || e.index >= m.structs[a.structIndex].fields.Size())
                        return Fail("a field is read from something that is not a struct with that field");
                    if (!SameData(e.type, m.structs[a.structIndex].fields[e.index].type))
                        return Fail(Format("field {} has the wrong type", m.structs[a.structIndex].fields[e.index].name));
                    break;
                case ExprKind::Swizzle:
                    if (!e.a || e.swizzleCount < 1 || e.swizzleCount > 4 || !(a.IsScalar() || a.IsVector()))
                        return Fail("a swizzle of something that is not a vector");
                    for (u32 i = 0; i < e.swizzleCount; i++)
                        if (e.swizzle[i] >= a.rows)
                            return Fail("a swizzle names a component the vector does not have");
                    break;
                case ExprKind::Index:
                    if (!e.a || !e.b || !(a.IsVector() || a.IsMatrix() || a.base == BaseType::Array || a.base == BaseType::Buffer || a.base == BaseType::Image))
                        return Fail("an index into something that has no elements");
                    break;
                case ExprKind::Unary: case ExprKind::Cast:
                    if (!e.a)
                        return Fail("an operation without an operand");
                    if (!a.IsNumeric() && !a.IsMatrix())
                        return Fail("an operation on a value that is not a number, vector or matrix");
                    break;
                case ExprKind::Binary:
                {
                    if (!e.a || !e.b)
                        return Fail("a binary operation without both operands");
                    Type b = f->exprs[e.b].type;
                    if ((!a.IsNumeric() && !a.IsMatrix()) || (!b.IsNumeric() && !b.IsMatrix()))
                        return Fail("an operation on a value that is not a number, vector or matrix");
                    break;
                }
                case ExprKind::Call:
                {
                    if (e.index >= self)
                        return Fail("a call to a function that is not translated before its caller (recursion)");
                    const Function& callee = m.functions[e.index];
                    if (callee.params.Size() != e.args.Size())
                        return Fail(Format("{} takes {} arguments and gets {}", callee.name, callee.params.Size(), e.args.Size()));
                    for (usize i = 0; i < e.args.Size(); i++)
                        if (callee.params[i].mode != ParamMode::In && !Place(e.args[i]))
                            return Fail(Format("argument {} of {} is written but is not a variable", i + 1, callee.name));
                    break;
                }
                case ExprKind::Intrinsic:
                    if (e.intrinsic >= Intrinsic::Count)
                        return Fail("an unknown intrinsic");
                    if (e.args.Size() != IntrinsicArity(e.intrinsic))
                        return Fail(Format("{} takes {} arguments and gets {}", IntrinsicName(e.intrinsic), IntrinsicArity(e.intrinsic), e.args.Size()));
                    break;
                default: break;
                }
                return true;
            }

            bool Statements(const Array<Stmt>& stmts, u32 loops)
            {
                for (const Stmt& s : stmts)
                {
                    switch (s.kind)
                    {
                    case StmtKind::Assign:
                        if (!Expression(s.a) || !Expression(s.b))
                            return false;
                        if (!Place(s.a))
                            return Fail("an assignment to something that is not a variable");
                        if (!SameData(f->exprs[s.a].type, f->exprs[s.b].type))
                            return Fail("an assignment of a value of another type");
                        break;
                    case StmtKind::Evaluate:
                        if (!Expression(s.a))
                            return false;
                        break;
                    case StmtKind::Return:
                        if (f->returnType.IsVoid() != (s.a == 0))
                            return Fail("a return does not fit the return type");
                        if (s.a && (!Expression(s.a) || !SameData(f->exprs[s.a].type, f->returnType)))
                            return Fail("a return value of another type");
                        break;
                    case StmtKind::If:
                        if (!Expression(s.a))
                            return false;
                        if (f->exprs[s.a].type != kBool)
                            return Fail("a condition that is not a bool");
                        if (!Statements(s.body, loops) || !Statements(s.elseBody, loops))
                            return false;
                        break;
                    case StmtKind::Loop:
                        if (!Statements(s.body, loops + 1))
                            return false;
                        break;
                    case StmtKind::Break: case StmtKind::Continue:
                        if (loops == 0)
                            return Fail("a break or continue outside a loop");
                        break;
                    case StmtKind::Discard:
                        if (m.stage != Stage::Fragment)
                            return Fail("discard outside a fragment shader");
                        break;
                    }
                }
                return true;
            }

            bool Run()
            {
                for (const StructType& s : m.structs)
                    for (const StructField& field : s.fields)
                        if (!TypeOk(field.type))
                            return false;
                for (const Global& g : m.globals)
                    if (!TypeOk(g.type) || !TypeOk(g.element))
                        return false;
                if (m.functions.Empty() || m.entry >= m.functions.Size())
                    return Fail("the entry point does not exist");
                for (self = 0; self < m.functions.Size(); self++)
                {
                    f = &m.functions[self];
                    if (f->exprs.Empty())
                        return Fail("the expression arena has no null expression");
                    for (const Variable& v : f->params)
                        if (!TypeOk(v.type))
                            return false;
                    for (const Variable& v : f->locals)
                        if (!TypeOk(v.type))
                            return false;
                    if (!Statements(f->body, 0))
                        return false;
                }
                return true;
            }
        };

        // ---------------------------------------------------------------- printing

        void PrintType(const Module& m, Type t, String& out)
        {
            static constexpr const char* kBases[] = {"void",    "bool",      "int",      "uint",    "float",   "struct", "texture2d",
                                                     "texture2darray", "texture3d", "texturecube", "sampler", "buffer", "image", "array"};
            if (t.base == BaseType::Struct && t.structIndex < m.structs.Size())
            {
                out.Append(m.structs[t.structIndex].name);
                return;
            }
            out.Append(u32(t.base) < sizeof(kBases) / sizeof(kBases[0]) ? kBases[u32(t.base)] : "?");
            if (t.half)
                out.Append("h");
            if (t.cols > 1)
                FormatAppend(out, "{}x{}", u32(t.rows), u32(t.cols));
            else if (t.rows > 1)
                FormatAppend(out, "{}", u32(t.rows));
        }

        void PrintExpr(const Module& m, const Function& f, ExprId id, String& out)
        {
            static constexpr const char* kOps[] = {"-", "!", "~", "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "&&", "||", "==", "!=", "<", "<=", ">", ">="};
            if (id == 0 || id >= f.exprs.Size())
            {
                out.Append("?");
                return;
            }
            const Expr& e = f.exprs[id];
            auto list = [&]() {
                for (usize i = 0; i < e.args.Size(); i++)
                {
                    if (i)
                        out.Append(", ");
                    PrintExpr(m, f, e.args[i], out);
                }
            };
            switch (e.kind)
            {
            case ExprKind::ConstFloat: FormatAppend(out, "{}", e.number); break;
            case ExprKind::ConstInt: FormatAppend(out, "{}", e.integer); break;
            case ExprKind::ConstBool: out.Append(e.integer ? "true" : "false"); break;
            case ExprKind::Local: out.Append(e.index < f.locals.Size() ? StringView(f.locals[e.index].name) : StringView("?")); break;
            case ExprKind::Param: FormatAppend(out, "p_{}", e.index < f.params.Size() ? StringView(f.params[e.index].name) : StringView("?")); break;
            case ExprKind::Global: FormatAppend(out, "g_{}", e.index < m.globals.Size() ? StringView(m.globals[e.index].name) : StringView("?")); break;
            case ExprKind::Field:
            {
                PrintExpr(m, f, e.a, out);
                Type t = f.exprs[e.a].type;
                bool known = t.base == BaseType::Struct && t.structIndex < m.structs.Size() && e.index < m.structs[t.structIndex].fields.Size();
                FormatAppend(out, ".{}", known ? StringView(m.structs[t.structIndex].fields[e.index].name) : StringView("?"));
                break;
            }
            case ExprKind::Swizzle:
                PrintExpr(m, f, e.a, out);
                out.Append('.');
                for (u32 i = 0; i < e.swizzleCount && i < 4; i++)
                    out.Append("xyzw"[e.swizzle[i] & 3]);
                break;
            case ExprKind::Index:
                PrintExpr(m, f, e.a, out);
                out.Append('[');
                PrintExpr(m, f, e.b, out);
                out.Append(']');
                break;
            case ExprKind::Unary:
                out.Append(u32(e.op) < sizeof(kOps) / sizeof(kOps[0]) ? kOps[u32(e.op)] : "?");
                PrintExpr(m, f, e.a, out);
                break;
            case ExprKind::Binary:
                out.Append('(');
                PrintExpr(m, f, e.a, out);
                FormatAppend(out, " {} ", u32(e.op) < sizeof(kOps) / sizeof(kOps[0]) ? kOps[u32(e.op)] : "?");
                PrintExpr(m, f, e.b, out);
                out.Append(')');
                break;
            case ExprKind::Cast: case ExprKind::Construct:
                PrintType(m, e.type, out);
                out.Append('(');
                if (e.kind == ExprKind::Cast)
                    PrintExpr(m, f, e.a, out);
                else
                    list();
                out.Append(')');
                break;
            case ExprKind::Call:
                FormatAppend(out, "{}(", e.index < m.functions.Size() ? StringView(m.functions[e.index].name) : StringView("?"));
                list();
                out.Append(')');
                break;
            case ExprKind::Intrinsic:
                FormatAppend(out, "{}(", IntrinsicName(e.intrinsic));
                list();
                out.Append(')');
                break;
            }
        }

        void PrintStmts(const Module& m, const Function& f, const Array<Stmt>& stmts, u32 depth, String& out)
        {
            for (const Stmt& s : stmts)
            {
                for (u32 i = 0; i < depth; i++)
                    out.Append("    ");
                switch (s.kind)
                {
                case StmtKind::Assign:
                    PrintExpr(m, f, s.a, out);
                    out.Append(" = ");
                    PrintExpr(m, f, s.b, out);
                    break;
                case StmtKind::Evaluate: PrintExpr(m, f, s.a, out); break;
                case StmtKind::Return:
                    out.Append("return");
                    if (s.a)
                    {
                        out.Append(' ');
                        PrintExpr(m, f, s.a, out);
                    }
                    break;
                case StmtKind::Discard: out.Append("discard"); break;
                case StmtKind::Break: out.Append("break"); break;
                case StmtKind::Continue: out.Append("continue"); break;
                case StmtKind::If: case StmtKind::Loop:
                    if (s.kind == StmtKind::If)
                    {
                        out.Append("if ");
                        PrintExpr(m, f, s.a, out);
                    }
                    else
                        out.Append("loop");
                    out.Append('\n');
                    PrintStmts(m, f, s.body, depth + 1, out);
                    if (!s.elseBody.Empty())
                    {
                        for (u32 i = 0; i < depth; i++)
                            out.Append("    ");
                        out.Append("else\n");
                        PrintStmts(m, f, s.elseBody, depth + 1, out);
                    }
                    continue;
                }
                out.Append('\n');
            }
        }

        bool SimplifyPass(Module& module, String&)
        {
            Simplify(module);
            return true;
        }

        constexpr Pass kPasses[] = {
            {"simplify", SimplifyPass, true},
            {"unroll", passes::UnrollLoops},
            // Unrolled bodies have constant counters to fold.
            {"simplify again", SimplifyPass},
            {"struct locals", passes::ReplaceStructLocals},
            {"copies", passes::PropagateCopies},
            {"common subexpressions", passes::EliminateCommonSubexpressions},
            {"dead code", passes::RemoveDeadCode},
        };
    }

    bool VerifyModule(const Module& module, String& error)
    {
        error.Clear();
        Verifier v{module, error};
        return v.Run();
    }

    void PrintModule(const Module& module, String& out)
    {
        for (usize i = 0; i < module.functions.Size(); i++)
        {
            const Function& f = module.functions[i];
            PrintType(module, f.returnType, out);
            FormatAppend(out, " {}(", f.name);
            for (usize p = 0; p < f.params.Size(); p++)
            {
                if (p)
                    out.Append(", ");
                static constexpr const char* kModes[] = {"", "out ", "inout "};
                out.Append(kModes[u32(f.params[p].mode)]);
                PrintType(module, f.params[p].type, out);
                FormatAppend(out, " p_{}", f.params[p].name);
            }
            out.Append(i == module.entry ? ") entry\n" : ")\n");
            for (const Variable& v : f.locals)
            {
                out.Append("    var ");
                PrintType(module, v.type, out);
                FormatAppend(out, " {}\n", v.name);
            }
            PrintStmts(module, f, f.body, 1, out);
        }
    }

    Span<const Pass> ShaderPasses() { return Span<const Pass>(kPasses, sizeof(kPasses) / sizeof(kPasses[0])); }

    bool RunPasses(Module& module, String& error, const PassOptions& options)
    {
        error.Clear();
        if (options.verify && !VerifyModule(module, error))
        {
            error = Format("the translated module is damaged: {}", error);
            return false;
        }
        for (const Pass& pass : ShaderPasses())
        {
            if (!pass.required && !options.optimize)
                continue;
            if (!pass.run(module, error))
                return false;
            if (options.dump)
                options.dump(pass.name, module, options.user);
            if (options.verify && !VerifyModule(module, error))
            {
                error = Format("the pass {} left a damaged module: {}", pass.name, error);
                return false;
            }
        }
        return true;
    }
}
