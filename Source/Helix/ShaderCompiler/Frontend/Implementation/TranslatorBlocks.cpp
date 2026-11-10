#include <Helix/ShaderCompiler/Frontend/Implementation/Translator.h>

namespace helix::shadercompiler::frontend
{
    void Translator::LeaveBlock(Method& mt, i32 target)
    {
        Method::BlockInfo& info = mt.info[usize(target)];
        if (!info.hasEntry)
        {
            info.hasEntry = true;
            for (const Item& item : mt.stack)
            {
                if (item.kind != ItemKind::Value)
                {
                    info.entry.Push(item);
                    continue;
                }
                Type t = TypeOf(mt, item.e);
                info.entry.Push({item.address ? item.e : Leaf(mt, ExprKind::Local, NewLocal(mt, t, true), t), item.address});
            }
        }
        for (usize i = 0; i < mt.stack.Size() && i < info.entry.Size(); i++)
        {
            if (mt.stack[i].kind != ItemKind::Value || info.entry[i].kind != ItemKind::Value)
            {
                if (mt.stack[i].kind != info.entry[i].kind ||
                    (mt.stack[i].index != info.entry[i].index && !(mt.stack[i].kind == ItemKind::Delegate && SameDelegate(mt, mt.stack[i].index, info.entry[i].index))))
                    Fail(mt.name, "a delegate depends on a condition; shaders call delegates that are known when they compile");
                continue;
            }
            if (mt.stack[i].address)
                continue;
            Stmt& s = mt.stmts->Emplace();
            s.kind = StmtKind::Assign;
            s.a = info.entry[i].e;
            s.b = Coerce(mt, mt.stack[i].e, TypeOf(mt, s.a));
        }
    }

    bool Translator::TranslateBlock(Method& mt, u32 blockIndex)
    {
        mt.stack = mt.info[blockIndex].entry;
        mt.stmts = &mt.blocks[blockIndex].stmts;
        u32 first = mt.info[blockIndex].first, last = mt.info[blockIndex].last;
        auto local = [&](u32 i) { return Leaf(mt, ExprKind::Local, i, mt.fn.locals[i].type); };
        // A ref local, or a Span over an inline array, stands for the place it was set to.
        auto loadLocal = [&](u32 slot) {
            if (slot < mt.isRef.Size() && mt.isRef[slot] == 3)
            {
                if (mt.localDelegate[slot] < 0)
                    return Fail(mt.name, "a delegate is used before it is set");
                mt.stack.Push({0, false, ItemKind::Delegate, mt.localDelegate[slot]});
                return true;
            }
            if (slot >= mt.isRef.Size() || !mt.isRef[slot])
            {
                Push(mt, local(slot));
                return true;
            }
            if (!mt.refTarget[slot])
                return Fail(mt.name, "a ref local is read before it is set");
            Push(mt, mt.refTarget[slot], true);
            return true;
        };
        auto storeLocal = [&](u32 slot) {
            Item item = Pop(mt);
            if (slot < mt.isRef.Size() && mt.isRef[slot] == 3)
            {
                if (item.kind != ItemKind::Delegate)
                    return Fail(mt.name, "a delegate variable must be set to a lambda, a local function or a static method");
                i32 old = mt.localDelegate[slot];
                if (old >= 0 && !SameDelegate(mt, old, item.index))
                    return Fail(mt.name, "a delegate variable is set to two different methods; shaders call delegates that are known when "
                                         "they compile");
                mt.localDelegate[slot] = item.index;
                return true;
            }
            if (item.kind != ItemKind::Value)
                return Fail(mt.name, "a delegate is stored in a variable of another type");
            if (slot >= mt.isRef.Size() || !mt.isRef[slot])
            {
                Assign(mt, local(slot), item.e);
                return true;
            }
            if (!item.address)
                return Fail(mt.name, "a ref local must refer to a variable, a field or an element");
            mt.refTarget[slot] = item.e;
            return true;
        };
        // IL arguments map to parameters; a delegate argument stands for its delegate.
        auto loadArg = [&](u32 k, bool address) {
            if (k < mt.argDelegate.Size() && mt.argDelegate[k] >= 0)
            {
                mt.stack.Push({0, false, ItemKind::Delegate, mt.argDelegate[k]});
                return true;
            }
            if (k >= mt.argParam.Size() || mt.argParam[k] < 0)
                return Fail(mt.name, "this of a lambda without captures is read");
            u32 p = u32(mt.argParam[k]);
            Push(mt, Leaf(mt, ExprKind::Param, p, mt.fn.params[p].type), address || mt.fn.params[p].mode != ParamMode::In);
            return true;
        };
        auto target = [&](u32 offset) { return offset < mt.blockOfOffset.Size() ? mt.blockOfOffset[offset] : -1; };
        auto finish = [&](FlowBlock::End end, ExprId value, i32 yes, i32 no) {
            // Blocks may have been appended, so the block is looked up again.
            mt.stmts = &mt.blocks[blockIndex].stmts;
            if (yes >= 0)
                LeaveBlock(mt, yes);
            if (no >= 0 && !mt.info[usize(no)].hasEntry)
            {
                // Both targets receive the same values; the temporaries of the first are reused.
                mt.info[usize(no)].hasEntry = true;
                mt.info[usize(no)].entry = yes >= 0 ? mt.info[usize(yes)].entry : mt.stack;
            }
            else if (no >= 0 && !mt.stack.Empty())
                LeaveBlock(mt, no);
            FlowBlock& block = mt.blocks[blockIndex];
            block.end = end;
            block.value = value;
            block.succ[0] = yes;
            block.succ[1] = no;
        };

        for (u32 i = first; i < last; i++)
        {
            const Instruction in = mt.code[i];
            u32 op = u32(in.opcode);
            location.offset = in.offset;
            bool lastInBlock = i + 1 == last;
            i32 next = i + 1 < mt.code.Size() ? target(mt.code[i + 1].offset) : -1;

            // Argument, local and constant shortcuts share one path with their long forms.
            bool handled = false;
            if (op >= u32(Opcode::Ldarg0) && op <= u32(Opcode::Ldarg3))
            {
                if (!loadArg(op - u32(Opcode::Ldarg0), false))
                    return false;
                handled = true;
            }
            else if (op >= u32(Opcode::Ldloc0) && op <= u32(Opcode::Ldloc3))
            {
                if (!loadLocal(op - u32(Opcode::Ldloc0)))
                    return false;
                handled = true;
            }
            else if (op >= u32(Opcode::Stloc0) && op <= u32(Opcode::Stloc3))
            {
                if (!storeLocal(op - u32(Opcode::Stloc0)))
                    return false;
                handled = true;
            }
            else if (op >= u32(Opcode::LdcI4M1) && op <= u32(Opcode::LdcI4_8))
            {
                Push(mt, ConstInt(mt, i64(op) - i64(Opcode::LdcI4_0), kInt));
                handled = true;
            }

            if (!handled)
            switch (in.opcode)
            {
            case Opcode::Nop: break;
            case Opcode::LdargS: case Opcode::Ldarg:
                if (!loadArg(u32(in.value), false))
                    return false;
                break;
            case Opcode::LdargaS: case Opcode::Ldarga:
                if (!loadArg(u32(in.value), true))
                    return false;
                break;
            case Opcode::StargS: case Opcode::Starg:
            {
                u32 k = u32(in.value);
                Item value = Pop(mt);
                if (k >= mt.argParam.Size() || mt.argParam[k] < 0 || value.kind != ItemKind::Value)
                    return Fail(mt.name, "a delegate parameter cannot be changed");
                u32 p = u32(mt.argParam[k]);
                Assign(mt, Leaf(mt, ExprKind::Param, p, mt.fn.params[p].type), value.e);
                break;
            }
            case Opcode::Ldnull: mt.stack.Push({0, false, ItemKind::Null}); break;
            case Opcode::Ldftn:
            {
                Token method(u32(in.value));
                DelegateInfo d;
                if (method.Is(Table::MethodSpec))
                {
                    Array<TypeSig> sigs;
                    if (!ParseMethodSpec(a->GetBlob(a->Cell(Table::MethodSpec, method.Row(), 1)), sigs))
                        return Fail(mt.name, "damaged generic method instantiation");
                    for (const TypeSig& s : sigs)
                    {
                        bool byRef = false;
                        if (!MapType(s, &mt, d.typeArgs.Emplace(), byRef, mt.name))
                            return Fail(mt.name, "a generic method is used with a type that shaders cannot use");
                    }
                    method = a->CodedCell(Table::MethodSpec, method.Row(), 0);
                }
                if (!method.Is(Table::MethodDef))
                    return Fail(mt.name, "a delegate of a method of another assembly is not available in shaders");
                d.assembly = current;
                d.target = method.Row();
                mt.delegates.Push(Move(d));
                mt.stack.Push({0, false, ItemKind::Function, i32(mt.delegates.Size() - 1)});
                break;
            }
            case Opcode::LdlocS: case Opcode::Ldloc:
                if (!loadLocal(u32(in.value)))
                    return false;
                break;
            case Opcode::LdlocaS: case Opcode::Ldloca:
                if (u32(in.value) < mt.isRef.Size() && mt.isRef[u32(in.value)] == 2)
                {
                    // The address of a Span over an inline array: its members reach the array.
                    if (!mt.refTarget[u32(in.value)])
                        return Fail(mt.name, "a span is read before it is set");
                    Push(mt, mt.refTarget[u32(in.value)], true);
                    break;
                }
                if (u32(in.value) < mt.isRef.Size() && mt.isRef[u32(in.value)])
                    return Fail(mt.name, "the address of a ref local is not available in shaders");
                Push(mt, local(u32(in.value)), true);
                break;
            case Opcode::StlocS: case Opcode::Stloc:
                if (!storeLocal(u32(in.value)))
                    return false;
                break;
            case Opcode::LdcI4S: case Opcode::LdcI4: Push(mt, ConstInt(mt, in.value, kInt)); break;
            case Opcode::LdcR4: case Opcode::LdcR8: Push(mt, ConstFloat(mt, in.number)); break;
            case Opcode::Dup:
            {
                Item top = Pop(mt);
                if (top.kind != ItemKind::Value)
                {
                    mt.stack.Push(top);
                    mt.stack.Push(top);
                    break;
                }
                ExprKind kind = mt.fn.exprs[top.e].kind;
                bool simple = top.address || kind == ExprKind::ConstFloat || kind == ExprKind::ConstInt || kind == ExprKind::ConstBool ||
                              kind == ExprKind::Local || kind == ExprKind::Param;
                if (!simple)
                    Spill(mt, top);
                mt.stack.Push(top);
                mt.stack.Push(top);
                break;
            }
            case Opcode::Pop: Pop(mt); break;

            case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::Div: case Opcode::DivUn: case Opcode::Rem:
            case Opcode::RemUn: case Opcode::And: case Opcode::Or: case Opcode::Xor: case Opcode::Shl: case Opcode::Shr:
            case Opcode::ShrUn:
            {
                static constexpr Op kOps[] = {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Div, Op::Rem, Op::Rem,
                                              Op::And, Op::Or,  Op::Xor, Op::Shl, Op::Shr, Op::Shr};
                bool asUnsigned = in.opcode == Opcode::DivUn || in.opcode == Opcode::RemUn || in.opcode == Opcode::ShrUn;
                ExprId y = Pop(mt).e, x = Pop(mt).e;
                Push(mt, Arithmetic(mt, kOps[op - u32(Opcode::Add)], x, y, asUnsigned));
                break;
            }
            case Opcode::Neg: case Opcode::Not:
            {
                ExprId x = Pop(mt).e;
                Push(mt, MakeUnary(mt.fn, in.opcode == Opcode::Neg ? Op::Neg : Op::BitNot, x, TypeOf(mt, x)));
                break;
            }
            case Opcode::ConvR4: case Opcode::ConvR8:
            {
                ExprId x = Pop(mt).e;
                Push(mt, TypeOf(mt, x).base == BaseType::Float ? x : Cast(mt, x, kFloat));
                break;
            }
            case Opcode::ConvRUn:
            {
                ExprId x = Pop(mt).e;
                Push(mt, Cast(mt, Coerce(mt, x, kUInt), kFloat));
                break;
            }
            case Opcode::ConvI4: case Opcode::ConvU4:
            {
                ExprId x = Pop(mt).e;
                Type to = in.opcode == Opcode::ConvI4 ? kInt : kUInt;
                Push(mt, TypeOf(mt, x) == to ? x : Cast(mt, x, to));
                break;
            }
            case Opcode::Ceq: case Opcode::Cgt: case Opcode::CgtUn: case Opcode::Clt: case Opcode::CltUn:
            {
                ExprId y = Pop(mt).e, x = Pop(mt).e;
                Op cmp = in.opcode == Opcode::Ceq ? Op::Eq : (in.opcode == Opcode::Cgt || in.opcode == Opcode::CgtUn) ? Op::Gt : Op::Lt;
                Push(mt, Compare(mt, cmp, x, y, in.opcode == Opcode::CgtUn || in.opcode == Opcode::CltUn));
                break;
            }

            case Opcode::Ldfld: case Opcode::Ldflda:
            {
                Item object = Pop(mt);
                ExprId field;
                if (!FieldExpr(mt, Token(u32(in.value)), object.e, field))
                    return false;
                Push(mt, field, in.opcode == Opcode::Ldflda);
                break;
            }
            case Opcode::Stfld:
            {
                Item value = Pop(mt);
                Item object = Pop(mt);
                if (value.kind != ItemKind::Value)
                    return Fail(mt.name, "a delegate cannot be stored in a field; it may only be called or passed to methods of the shader");
                ExprId field;
                if (!FieldExpr(mt, Token(u32(in.value)), object.e, field))
                    return false;
                Assign(mt, field, value.e);
                break;
            }
            case Opcode::Ldsfld: case Opcode::Ldsflda:
            {
                Token token(u32(in.value));
                if (token.Is(Table::Field) && IsLambdaHolder(a->DeclaringTypeOfField(token.Row())))
                {
                    // The instance lambdas without captures are called on; they never read it.
                    mt.stack.Push({0, false, ItemKind::Null});
                    break;
                }
                Item item;
                if (!StaticField(mt, Token(u32(in.value)), item))
                    return false;
                Push(mt, item.e, in.opcode == Opcode::Ldsflda && item.address);
                break;
            }
            // Reads through an address are the expression of the place itself.
            case Opcode::Ldobj: case Opcode::LdindR4: case Opcode::LdindI4: case Opcode::LdindU4: case Opcode::LdindI1:
            case Opcode::LdindU1: case Opcode::LdindR8:
                Push(mt, Pop(mt).e);
                break;
            case Opcode::Stobj: case Opcode::StindR4: case Opcode::StindI4: case Opcode::StindI1: case Opcode::StindR8:
            {
                ExprId value = Pop(mt).e;
                ExprId place = Pop(mt).e;
                Assign(mt, place, value);
                break;
            }
            case Opcode::Initobj:
            {
                ExprId place = Pop(mt).e;
                Expr zero;
                zero.kind = ExprKind::Construct;
                zero.type = TypeOf(mt, place);
                Assign(mt, place, AddExpr(mt.fn, Move(zero)));
                break;
            }
            case Opcode::Call: case Opcode::Callvirt: case Opcode::Newobj:
                if (!Call(mt, Token(u32(in.value)), in.opcode == Opcode::Newobj))
                    return false;
                break;

            case Opcode::Ret:
            {
                ExprId value = mt.fn.returnType.IsVoid() ? 0 : Coerce(mt, Pop(mt).e, mt.fn.returnType);
                mt.stack.Clear();
                finish(FlowBlock::End::Return, value, -1, -1);
                return true;
            }
            case Opcode::Br: case Opcode::BrS:
                finish(FlowBlock::End::Goto, 0, target(in.target), -1);
                return true;
            case Opcode::Brtrue: case Opcode::BrtrueS: case Opcode::Brfalse: case Opcode::BrfalseS:
            {
                ExprId condition = Coerce(mt, Pop(mt).e, kBool);
                if (in.opcode == Opcode::Brfalse || in.opcode == Opcode::BrfalseS)
                    condition = MakeNot(mt.fn, condition);
                finish(FlowBlock::End::Branch, condition, target(in.target), next);
                return true;
            }
            case Opcode::Beq: case Opcode::BeqS: case Opcode::Bge: case Opcode::BgeS: case Opcode::Bgt: case Opcode::BgtS:
            case Opcode::Ble: case Opcode::BleS: case Opcode::Blt: case Opcode::BltS: case Opcode::BneUn: case Opcode::BneUnS:
            case Opcode::BgeUn: case Opcode::BgeUnS: case Opcode::BgtUn: case Opcode::BgtUnS: case Opcode::BleUn:
            case Opcode::BleUnS: case Opcode::BltUn: case Opcode::BltUnS:
            {
                // Short and long forms are in the same order: beq bge bgt ble blt bne.un bge.un bgt.un ble.un blt.un.
                static constexpr Op kOps[] = {Op::Eq, Op::Ge, Op::Gt, Op::Le, Op::Lt, Op::Ne, Op::Ge, Op::Gt, Op::Le, Op::Lt};
                u32 k = op >= u32(Opcode::Beq) ? op - u32(Opcode::Beq) : op - u32(Opcode::BeqS);
                ExprId y = Pop(mt).e, x = Pop(mt).e;
                finish(FlowBlock::End::Branch, Compare(mt, kOps[k], x, y, k >= 6), target(in.target), next);
                return true;
            }
            case Opcode::Switch:
            {
                // A chain of branches, with the cases that share a target tested together.
                Item value = Pop(mt);
                value.e = Coerce(mt, value.e, kInt);
                if (mt.fn.exprs[value.e].kind != ExprKind::Local)
                    Spill(mt, value);
                Array<i32> targets;
                for (u32 c = 0; c < in.switchCount; c++)
                {
                    i32 t = target(in.SwitchTarget(c));
                    if (!targets.Find(t))
                        targets.Push(t);
                }
                Array<Item> carried = mt.stack;
                u32 link = blockIndex;
                for (usize t = 0; t < targets.Size(); t++)
                {
                    ExprId condition = 0;
                    for (u32 c = 0; c < in.switchCount; c++)
                    {
                        if (target(in.SwitchTarget(c)) != targets[t])
                            continue;
                        ExprId test = MakeBinary(mt.fn, Op::Eq, value.e, ConstInt(mt, c, kInt), kBool);
                        condition = condition ? MakeBinary(mt.fn, Op::LogicalOr, condition, test, kBool) : test;
                    }
                    i32 rest = next;
                    if (t + 1 < targets.Size())
                    {
                        mt.blocks.Emplace();
                        Method::BlockInfo& chain = mt.info.Emplace();
                        chain.done = chain.hasEntry = true;
                        chain.entry = carried;
                        rest = i32(mt.blocks.Size() - 1);
                    }
                    u32 saved = blockIndex;
                    blockIndex = link;
                    mt.stack = carried;
                    finish(FlowBlock::End::Branch, condition, targets[t], rest);
                    blockIndex = saved;
                    link = u32(rest);
                }
                if (targets.Empty())
                    finish(FlowBlock::End::Goto, 0, next, -1);
                return true;
            }
            default:
                return Fail(mt.name, Format("the instruction {} is not available in shaders", OpcodeName(in.opcode)));
            }
            if (lastInBlock)
            {
                finish(FlowBlock::End::Goto, 0, next, -1);
                return true;
            }
        }
        return Fail(mt.name, "a block ends without a successor");
    }

    bool Translator::TranslateMethod(u32 row, const Array<Type>& typeArgs, const Array<Binding>& bindings, Function& out)
    {
        // Errors point into this method until it is done, then into the caller again.
        struct Restore
        {
            Location& location;
            Location saved;
            ~Restore() { location = saved; }
        } restore{location, location};
        location = {current, row, 0};
        Method mt;
        // Expression 0 is the null expression, also in a function without expressions.
        mt.fn.exprs.Emplace();
        MethodInfo info = a->Method(row);
        mt.name = info.name;
        mt.typeArgs = typeArgs;
        MethodBody body;
        if (!ParseMethodSignature(info.signature, mt.sig) || !a->Body(row, body))
            return Fail(mt.name, "the method has no body that can be read");
        if (body.hasExceptionRegions)
            return Fail(mt.name, "try, catch and finally are not available in shaders");
        bool byRef = false;
        mt.fn.loops = Has(Token(Table::MethodDef, row), "UnrollAttribute") ? LoopHint::Unroll
                      : Has(Token(Table::MethodDef, row), "LoopAttribute")  ? LoopHint::Keep
                                                                           : LoopHint::Auto;
        if (!MapType(mt.sig.returnType, &mt, mt.fn.returnType, byRef, mt.name) || byRef)
            return Fail(mt.name, "the return type cannot be used in shaders");

        if (mt.sig.hasThis && IsLambdaHolder(a->DeclaringTypeOfMethod(row)))
        {
            // Lambdas without captures are instance methods of a class without fields; this is never read.
            mt.argParam.Push(-1);
            mt.argDelegate.Push(-1);
        }
        else if (mt.sig.hasThis)
        {
            i32 self = StructFor(a->DeclaringTypeOfMethod(row), mt.name);
            if (self < 0)
                return false;
            Variable& v = mt.fn.params.Emplace();
            v.name = "self";
            v.type = Type::Struct(u32(self));
            v.mode = ParamMode::InOut;
            mt.argParam.Push(0);
            mt.argDelegate.Push(-1);
        }
        for (usize i = 0; i < mt.sig.params.Size(); i++)
        {
            u32 arg = u32(mt.argParam.Size());
            if (IsDelegateType(mt.sig.params[i]))
            {
                const Binding* b = nullptr;
                for (const Binding& x : bindings)
                    if (x.arg == arg)
                        b = &x;
                if (!b)
                    return Fail(mt.name, Format("parameter {} is a delegate; it must receive a lambda, a local function or a static method "
                                                "known when the shader compiles",
                                                i + 1));
                DelegateInfo d;
                d.assembly = b->assembly;
                d.target = b->target;
                d.typeArgs = b->typeArgs;
                mt.argParam.Push(-1);
                if (b->closure >= 0)
                {
                    Variable& v = mt.fn.params.Emplace();
                    v.name = Format("closure{}", i);
                    v.type = Type::Struct(u32(b->closure));
                    v.mode = ParamMode::InOut;
                    d.closure = Leaf(mt, ExprKind::Param, u32(mt.fn.params.Size() - 1), v.type);
                }
                mt.delegates.Push(Move(d));
                mt.argDelegate.Push(i32(mt.delegates.Size() - 1));
                continue;
            }
            mt.argParam.Push(i32(mt.fn.params.Size()));
            mt.argDelegate.Push(-1);
            Variable& v = mt.fn.params.Emplace();
            byRef = false;
            if (!MapType(mt.sig.params[i], &mt, v.type, byRef, mt.name))
                return Fail(mt.name, Format("parameter {} has a type that shaders cannot use", i + 1));
            v.name = Format("a{}", i);
            v.mode = byRef ? ParamMode::InOut : ParamMode::In;
            for (u32 p = info.paramBegin; p < info.paramEnd; p++)
            {
                ParamInfo pi = a->Param(p);
                if (pi.sequence != i + 1)
                    continue;
                v.name = pi.name;
                // Flag 2 is `out`, flag 1 is `in` (a read only reference).
                if (byRef)
                    v.mode = (pi.flags & 2) ? ParamMode::Out : (pi.flags & 1) ? ParamMode::In : ParamMode::InOut;
                v.semantic = ReadSemantic(Token(Table::Param, p), v.semanticIndex, v.flat);
            }
        }
        if (!body.localsSignature.IsNull())
        {
            Array<TypeSig> locals;
            if (!body.localsSignature.Is(Table::StandAloneSig) || !ParseLocalsSignature(a->StandAloneSignature(body.localsSignature.Row()), locals))
                return Fail(mt.name, "damaged local variable signature");
            for (const TypeSig& s : locals)
            {
                Type t;
                byRef = false;
                // A Span local only ever views an inline array; it stands for the array.
                bool span = IsSpanType(s), delegate = IsDelegateType(s);
                if (delegate)
                    t = kInt;
                else if (span && !MapType(s.args[0], &mt, t, byRef, mt.name))
                    t = kInt;
                else if (!span && !MapType(s, &mt, t, byRef, mt.name))
                    return Fail(mt.name, "a local variable has a type that shaders cannot use");
                mt.isRef.Push(delegate ? 3 : span ? 2 : byRef ? 1 : 0);
                mt.refTarget.Push(0);
                mt.localDelegate.Push(-1);
                NewLocal(mt, t, false);
            }
        }

        // Decode and cut into blocks at branch targets and after branches.
        Array<u8> leader;
        leader.Resize(body.code.Size() + 1, u8(0));
        leader[0] = 1;
        Instruction in;
        for (u32 at = 0; at < body.code.Size();)
        {
            if (!DecodeInstruction(body.code, at, in))
                return Fail(mt.name, "unknown IL instruction");
            mt.code.Push(in);
        }
        // The C# compiler caches lambdas without captures and method groups in static fields:
        //   ldsfld cache; dup; brtrue L; pop; <make the delegate>; dup; stsfld cache; [br L;] L:
        // Shaders make the delegate every time, which is the same value.
        for (usize i = 0; i + 3 < mt.code.Size(); i++)
        {
            const Instruction& c = mt.code[i];
            Token cache(u32(c.value));
            if (c.opcode != Opcode::Ldsfld || !cache.Is(Table::Field) || !IsLambdaHolder(a->DeclaringTypeOfField(cache.Row())) ||
                mt.code[i + 1].opcode != Opcode::Dup || (mt.code[i + 2].opcode != Opcode::Brtrue && mt.code[i + 2].opcode != Opcode::BrtrueS) ||
                mt.code[i + 3].opcode != Opcode::Pop)
                continue;
            u32 label = mt.code[i + 2].target;
            for (usize j = i + 4; j + 2 < mt.code.Size(); j++)
                if (mt.code[j].opcode == Opcode::Dup && mt.code[j + 1].opcode == Opcode::Stsfld && mt.code[j + 1].value == c.value)
                {
                    const Instruction& after = mt.code[j + 2];
                    bool jumps = (after.opcode == Opcode::Br || after.opcode == Opcode::BrS) && after.target == label;
                    if (after.offset == label || jumps)
                        for (usize k : {i, i + 1, i + 2, i + 3, j, j + 1})
                        {
                            mt.code[k].opcode = Opcode::Nop;
                            mt.code[k].operand = OperandKind::None;
                        }
                    break;
                }
        }
        // Damaged IL may name locals and arguments the method does not have.
        for (const Instruction& i : mt.code)
        {
            u32 op = u32(i.opcode);
            bool arg = i.opcode == Opcode::LdargS || i.opcode == Opcode::Ldarg || i.opcode == Opcode::LdargaS || i.opcode == Opcode::Ldarga ||
                       i.opcode == Opcode::StargS || i.opcode == Opcode::Starg || (op >= u32(Opcode::Ldarg0) && op <= u32(Opcode::Ldarg3));
            bool loc = i.opcode == Opcode::LdlocS || i.opcode == Opcode::Ldloc || i.opcode == Opcode::LdlocaS || i.opcode == Opcode::Ldloca ||
                       i.opcode == Opcode::StlocS || i.opcode == Opcode::Stloc || (op >= u32(Opcode::Ldloc0) && op <= u32(Opcode::Ldloc3)) ||
                       (op >= u32(Opcode::Stloc0) && op <= u32(Opcode::Stloc3));
            u64 index = op >= u32(Opcode::Ldarg0) && op <= u32(Opcode::Ldarg3)   ? op - u32(Opcode::Ldarg0)
                        : op >= u32(Opcode::Ldloc0) && op <= u32(Opcode::Ldloc3) ? op - u32(Opcode::Ldloc0)
                        : op >= u32(Opcode::Stloc0) && op <= u32(Opcode::Stloc3) ? op - u32(Opcode::Stloc0)
                                                                                 : u64(i.value);
            if ((arg && index >= mt.argParam.Size()) || (loc && index >= mt.isRef.Size()))
                return Fail(mt.name, Format("damaged IL at offset {}: there is no {} {}", i.offset, arg ? "argument" : "local", index));
        }
        for (const Instruction& decoded : mt.code)
        {
            in = decoded;
            u32 at = in.offset + in.size;
            bool branch = in.operand == OperandKind::Branch8 || in.operand == OperandKind::Branch32;
            if (branch || in.opcode == Opcode::Switch || in.opcode == Opcode::Ret)
                leader[at] = 1;
            if (branch && in.target < leader.Size())
                leader[in.target] = 1;
            for (u32 c = 0; c < in.switchCount; c++)
                if (in.SwitchTarget(c) < leader.Size())
                    leader[in.SwitchTarget(c)] = 1;
        }
        mt.blockOfOffset.Resize(body.code.Size() + 1, -1);
        for (u32 i = 0; i < mt.code.Size(); i++)
        {
            if (leader[mt.code[i].offset])
            {
                if (!mt.info.Empty())
                    mt.info.Back().last = i;
                mt.blocks.Emplace();
                mt.info.Emplace().first = i;
                mt.blockOfOffset[mt.code[i].offset] = i32(mt.blocks.Size() - 1);
            }
        }
        if (mt.info.Empty())
            return Fail(mt.name, "the method is empty");
        mt.info.Back().last = u32(mt.code.Size());
        for (const Instruction& i : mt.code)
            if ((i.operand == OperandKind::Branch8 || i.operand == OperandKind::Branch32) &&
                (i.target >= mt.blockOfOffset.Size() || mt.blockOfOffset[i.target] < 0))
                return Fail(mt.name, "a branch leaves the method");

        // Translate reachable blocks; a block is reached after one of its predecessors fixed its entry stack.
        Array<u32> work;
        work.Push(0);
        mt.info[0].hasEntry = true;
        while (!work.Empty())
        {
            u32 b = work.Back();
            work.Pop();
            if (mt.info[b].done)
                continue;
            mt.info[b].done = true;
            usize before = mt.blocks.Size();
            if (!TranslateBlock(mt, b))
                return false;
            for (i32 s : mt.blocks[b].succ)
                if (s >= 0)
                    work.Push(u32(s));
            // Chain blocks of a switch are complete already, but their targets still need a visit.
            for (usize extra = before; extra < mt.blocks.Size(); extra++)
                for (i32 s : mt.blocks[extra].succ)
                    if (s >= 0)
                        work.Push(u32(s));
        }
        if (!error.Empty())
            return false;
        for (FlowBlock& block : mt.blocks)
            for (i32& s : block.succ)
                if (s >= i32(mt.blocks.Size()))
                    return Fail(mt.name, "damaged control flow");
        String flowError;
        if (!StructureFlow(mt.blocks, mt.fn, mt.fn.body, flowError))
            return Fail(mt.name, flowError);
        for (usize p = 0; p < mt.fn.params.Size(); p++)
            if (mt.fn.params[p].mode == ParamMode::InOut && (p >= mt.written.Size() || !mt.written[p]))
                mt.fn.params[p].mode = ParamMode::In;
        out = Move(mt.fn);
        return true;
    }
}
