#include <Helix/ShaderCompiler/Frontend/Implementation/Translator.h>

namespace helix::shadercompiler::frontend
{
    ExprId Translator::Leaf(Method& mt, ExprKind kind, u32 index, Type type)
    {
        Expr e;
        e.kind = kind;
        e.index = index;
        e.type = type;
        return AddExpr(mt.fn, Move(e));
    }

    ExprId Translator::ConstInt(Method& mt, i64 value, Type type)
    {
        Expr e;
        e.kind = type.base == BaseType::Bool ? ExprKind::ConstBool : ExprKind::ConstInt;
        e.integer = type.base == BaseType::Bool ? (value != 0) : type.base == BaseType::UInt ? i64(u32(value)) : value;
        e.type = type;
        return AddExpr(mt.fn, Move(e));
    }

    ExprId Translator::ConstFloat(Method& mt, f64 value)
    {
        Expr e;
        e.kind = ExprKind::ConstFloat;
        e.number = value;
        e.type = kFloat;
        return AddExpr(mt.fn, Move(e));
    }

    ExprId Translator::Cast(Method& mt, ExprId value, Type to)
    {
        Expr e;
        e.kind = ExprKind::Cast;
        e.a = value;
        e.type = to;
        return AddExpr(mt.fn, Move(e));
    }

    ExprId Translator::Coerce(Method& mt, ExprId value, Type to)
    {
        Expr e = mt.fn.exprs[value];
        Type from = e.type;
        if (from == to || !to.IsScalar() || !from.IsScalar())
            return value;
        if ((e.kind == ExprKind::ConstInt || e.kind == ExprKind::ConstBool) && to.base != BaseType::Float)
            return ConstInt(mt, e.integer, to);
        if (to.base == BaseType::Bool)
            return MakeBinary(mt.fn, Op::Ne, value, from.base == BaseType::Float ? ConstFloat(mt, 0.0) : ConstInt(mt, 0, from), kBool);
        if (from.base == BaseType::Float && to.base == BaseType::Float)
            return value;
        return Cast(mt, value, to);
    }

    u32 Translator::NewLocal(Method& mt, Type type, bool temporary)
    {
        Variable& v = mt.fn.locals.Emplace();
        v.type = type;
        v.name = Format(temporary ? "t{}" : "l{}", mt.fn.locals.Size() - 1);
        mt.temporary.Push(temporary ? 1 : 0);
        return u32(mt.fn.locals.Size() - 1);
    }

    bool Translator::ReadsState(const Method& mt, ExprId id)
    {
        const Expr& e = mt.fn.exprs[id];
        switch (e.kind)
        {
        case ExprKind::ConstFloat: case ExprKind::ConstInt: case ExprKind::ConstBool: return false;
        case ExprKind::Local: return !mt.temporary[e.index];
        case ExprKind::Param: case ExprKind::Global: case ExprKind::Call: return true;
        case ExprKind::Intrinsic:
            if (e.intrinsic == Intrinsic::BindlessLoad)
                return true;
            break;
        default: break;
        }
        if ((e.a && ReadsState(mt, e.a)) || (e.b && ReadsState(mt, e.b)))
            return true;
        for (ExprId arg : e.args)
            if (ReadsState(mt, arg))
                return true;
        return false;
    }

    void Translator::Spill(Method& mt, Item& item)
    {
        u32 local = NewLocal(mt, TypeOf(mt, item.e), true);
        Stmt& s = mt.stmts->Emplace();
        s.kind = StmtKind::Assign;
        s.a = Leaf(mt, ExprKind::Local, local, TypeOf(mt, item.e));
        s.b = item.e;
        item.e = s.a;
    }

    void Translator::Flush(Method& mt)
    {
        for (Item& item : mt.stack)
            if (item.kind == ItemKind::Value && !item.address && ReadsState(mt, item.e))
                Spill(mt, item);
    }

    void Translator::MarkWritten(Method& mt, ExprId place)
    {
        while (place)
        {
            const Expr& e = mt.fn.exprs[place];
            if (e.kind == ExprKind::Param)
            {
                if (e.index >= mt.written.Size())
                    mt.written.Resize(e.index + 1, u8(0));
                mt.written[e.index] = 1;
                return;
            }
            if (e.kind != ExprKind::Field && e.kind != ExprKind::Swizzle && e.kind != ExprKind::Index)
                return;
            place = e.a;
        }
    }

    void Translator::Assign(Method& mt, ExprId target, ExprId value)
    {
        MarkWritten(mt, target);
        Flush(mt);
        Stmt& s = mt.stmts->Emplace();
        s.kind = StmtKind::Assign;
        s.a = target;
        s.b = Coerce(mt, value, TypeOf(mt, target));
    }

    Item Translator::Pop(Method& mt)
    {
        if (mt.stack.Empty())
            return {};
        Item item = mt.stack.Back();
        mt.stack.Pop();
        return item;
    }

    ExprId Translator::Swizzle(Method& mt, ExprId value, const u8* components, u32 count)
    {
        Type t = TypeOf(mt, value);
        Expr e;
        e.kind = ExprKind::Swizzle;
        e.a = value;
        e.swizzleCount = u8(count);
        for (u32 i = 0; i < count; i++)
            e.swizzle[i] = components[i];
        e.type = count == 1 ? t.ScalarType() : Type::Vector(t.base, count);
        return AddExpr(mt.fn, Move(e));
    }

    ExprId Translator::Index(Method& mt, ExprId value, ExprId index)
    {
        Type t = TypeOf(mt, value);
        Expr e;
        e.kind = ExprKind::Index;
        e.a = value;
        e.b = Coerce(mt, index, kInt);
        e.type = t.IsMatrix() ? Type::Vector(BaseType::Float, t.rows) : t.ScalarType();
        return AddExpr(mt.fn, Move(e));
    }

    void Translator::Unify(Method& mt, ExprId& x, ExprId& y, bool asUnsigned)
    {
        Type tx = TypeOf(mt, x), ty = TypeOf(mt, y);
        bool cx = mt.fn.exprs[x].kind == ExprKind::ConstInt, cy = mt.fn.exprs[y].kind == ExprKind::ConstInt;
        if (!(tx == ty) && tx.IsScalar() && ty.IsScalar())
        {
            if (cx && !cy)
                x = Coerce(mt, x, ty);
            else if (cy)
                y = Coerce(mt, y, tx);
            else if (tx.base == BaseType::Bool)
                x = Coerce(mt, x, ty);
            else if (ty.base == BaseType::Bool)
                y = Coerce(mt, y, tx);
            else if (tx.IsInteger() && ty.IsInteger())
            {
                x = Coerce(mt, x, kUInt);
                y = Coerce(mt, y, kUInt);
            }
        }
        if (asUnsigned && TypeOf(mt, x).base == BaseType::Int)
        {
            x = Coerce(mt, x, kUInt);
            y = Coerce(mt, y, kUInt);
        }
    }

    ExprId Translator::Arithmetic(Method& mt, Op op, ExprId x, ExprId y, bool asUnsigned)
    {
        if (op == Op::Shl || op == Op::Shr)
        {
            if (asUnsigned && TypeOf(mt, x).base == BaseType::Int)
                x = Coerce(mt, x, kUInt);
            return MakeBinary(mt.fn, op, x, Coerce(mt, y, kInt), TypeOf(mt, x));
        }
        Unify(mt, x, y, asUnsigned);
        Type t = TypeOf(mt, x);
        if (t.base == BaseType::Bool && (op == Op::And || op == Op::Or || op == Op::Xor))
            return MakeBinary(mt.fn, op == Op::And ? Op::LogicalAnd : op == Op::Or ? Op::LogicalOr : Op::Ne, x, y, kBool);
        return MakeBinary(mt.fn, op, x, y, t);
    }

    ExprId Translator::Compare(Method& mt, Op op, ExprId x, ExprId y, bool unordered)
    {
        const Expr ex = mt.fn.exprs[x], ey = mt.fn.exprs[y];
        // Tests of a bool against 0 or 1 are the bool itself or its negation.
        if (ex.type.base == BaseType::Bool && ey.kind == ExprKind::ConstInt && ex.type.IsScalar())
        {
            bool same = (op == Op::Eq) == (ey.integer != 0) || (op == Op::Gt && ey.integer == 0) || (op == Op::Ne && ey.integer == 0);
            if (op == Op::Eq || op == Op::Ne || op == Op::Gt)
                return same ? x : MakeNot(mt.fn, x);
        }
        // cgt.un against zero is the "not zero" test.
        if (unordered && op == Op::Gt && ey.kind == ExprKind::ConstInt && ey.integer == 0 && ex.type.IsInteger())
            return MakeBinary(mt.fn, Op::Ne, x, Coerce(mt, y, ex.type), kBool);
        bool isFloat = ex.type.base == BaseType::Float;
        Unify(mt, x, y, unordered && !isFloat);
        if (unordered && isFloat && op != Op::Ne && op != Op::Eq)
        {
            static constexpr Op kOpposite[] = {Op::Ge, Op::Gt, Op::Le, Op::Lt};
            return MakeUnary(mt.fn, Op::Not, MakeBinary(mt.fn, kOpposite[u32(op) - u32(Op::Lt)], x, y, kBool), kBool);
        }
        return MakeBinary(mt.fn, op, x, y, kBool);
    }
}
