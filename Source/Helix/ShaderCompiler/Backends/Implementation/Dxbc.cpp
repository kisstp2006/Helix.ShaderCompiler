#include <Helix/ShaderCompiler/Backends/Dxbc.h>
#include <Helix/ShaderCompiler/Backends/Implementation/UniformLayout.h>

#include <Helix/Core/Hashing/Md5.h>

#include <cstring>

namespace helix::shadercompiler
{
    namespace
    {
        // Opcodes of the tokenized shader format (shader model 4).
        enum Opcode : u32
        {
            kAdd = 0, kAnd = 1, kBreak = 2, kBreakc = 3, kContinue = 7, kDerivRtx = 11, kDerivRty = 12, kDiscard = 13, kDiv = 14,
            kDp2 = 15, kDp3 = 16, kDp4 = 17, kElse = 18, kEndif = 21, kEndloop = 22, kEq = 24, kExp = 25, kFrc = 26, kFtoi = 27,
            kFtou = 28, kGe = 29, kIadd = 30, kIf = 31, kIeq = 32, kIge = 33, kIlt = 34, kImax = 36, kImin = 37, kImul = 38,
            kIne = 39, kIneg = 40, kIshl = 41, kIshr = 42, kItof = 43, kLd = 45, kLog = 47, kLoop = 48, kLt = 49, kMad = 50,
            kMin = 51, kMax = 52, kMov = 54, kMovc = 55, kMul = 56, kNe = 57, kNot = 59, kOr = 60, kResinfo = 61, kRet = 62,
            kRoundNe = 64, kRoundNi = 65, kRoundPi = 66, kRoundZ = 67, kRsq = 68, kSample = 69, kSampleL = 72, kSqrt = 75,
            kSincos = 77, kUdiv = 78, kUlt = 79, kUge = 80, kUmax = 83, kUmin = 84, kUshr = 85, kUtof = 86, kXor = 87,
            kDclResource = 88, kDclConstantBuffer = 89, kDclSampler = 90, kDclInput = 95, kDclInputSgv = 96, kDclInputPs = 98,
            kDclInputPsSiv = 100, kDclOutput = 101, kDclOutputSiv = 103, kDclTemps = 104,
            kBufinfo = 121, kDclThreadGroup = 155, kDclUavStructured = 158, kDclResourceStructured = 162, kLdStructured = 167, kStoreStructured = 168,
        };
        constexpr u32 kTestNonZero = 1u << 18;
        constexpr u32 kSaturate = 1u << 13;

        u32 Bits(f32 v)
        {
            u32 b;
            std::memcpy(&b, &v, 4);
            return b;
        }

        // A source operand: up to four components of a register, a constant buffer vector or immediate values.
        struct Src
        {
            enum Kind : u8
            {
                Temp,
                Input,
                Immediate,
                Buffer,
                // The dispatch thread id of a compute shader.
                ThreadId,
            };
            Kind kind = Temp;
            u8 count = 1;
            // 1 negates, 2 takes the absolute value.
            u8 modifier = 0;
            bool isFloat = true;
            u8 swizzle[4] = {0, 1, 2, 3};
            u32 reg = 0;
            u32 index = 0;
            u32 imm[4] = {};

            u8 Component(u32 i) const { return swizzle[i < count ? i : count - 1u]; }
        };

        // Where a store goes: components of a temp or output register.
        struct Dst
        {
            u32 reg = 0;
            u8 comps[4] = {0, 1, 2, 3};
            u8 count = 1;
            bool output = false;
        };

        struct Value
        {
            Type type;
            Array<Src> parts;
        };

        struct SignatureElement
        {
            const char* name;
            u32 index, systemValue, componentType, reg, mask;
            bool unused = false;
        };

        struct Generator
        {
            const Module& m;
            String& error;
            Array<u32> code;
            u32 tempTop = 0, tempMax = 0, lastOpcode = ~0u;
            Array<u8> hasLoop;

            // One inlined copy of a function: its variables have their own registers.
            struct Instance
            {
                const Function* fn = nullptr;
                Array<u32> paramReg, localReg;
                u32 retReg = 0;
                i32 flagReg = -1;
                u32 loopDepth = 0;
                bool entry = false;
            };
            Array<Dst> entryOutputs;

            struct Resource
            {
                u32 slot, size;
            };
            Array<Resource> buffers;
            Array<u32> textures;
            // Storage buffers in use, as indices of the module's globals.
            Array<u32> storages;
            bool threadId = false;

            Generator(const Module& module, String& e) : m(module), error(e) {}

            bool Fail(StringView message)
            {
                if (error.Empty())
                    error = message;
                return false;
            }

            // ---------------------------------------------------------------- encoding

            struct Instruction
            {
                Array<u32> words;
                explicit Instruction(u32 opcode) { words.Push(opcode); }
            };

            void Finish(Instruction& in)
            {
                in.words[0] |= u32(in.words.Size()) << 24;
                lastOpcode = in.words[0] & 0x7FFu;
                code.Append(in.words.Data(), in.words.Size());
            }

            static void PutDst(Instruction& in, const Dst& d)
            {
                u32 mask = 0;
                for (u32 i = 0; i < d.count; i++)
                    mask |= 1u << d.comps[i];
                in.words.Push((d.output ? 0x00102002u : 0x00100002u) | (mask << 4));
                in.words.Push(d.reg);
            }

            static void PutNull(Instruction& in) { in.words.Push(0x0000D000u); }

            // `select` >= 0 encodes a one component operand (conditions); otherwise a four component swizzle.
            static void PutSrc(Instruction& in, const Src& s, i32 select = -1)
            {
                if (s.kind == Src::Immediate)
                {
                    in.words.Push(0x00004002u);
                    for (u32 i = 0; i < 4; i++)
                    {
                        u32 v = s.imm[s.Component(i)];
                        if (s.modifier == 2)
                            v = s.isFloat ? v & 0x7FFFFFFFu : (i32(v) < 0 ? u32(-i32(v)) : v);
                        else if (s.modifier == 1)
                            v = s.isFloat ? v ^ 0x80000000u : u32(-i32(v));
                        in.words.Push(v);
                    }
                    return;
                }
                u32 type = s.kind == Src::Temp ? 0u : s.kind == Src::Input ? 1u : s.kind == Src::ThreadId ? 0x20u : 8u;
                u32 dimension = s.kind == Src::Buffer ? 2u : s.kind == Src::ThreadId ? 0u : 1u;
                u32 selection;
                if (select >= 0)
                    selection = 0xAu | (u32(s.Component(u32(select))) << 4);
                else
                    selection = 0x6u | ((u32(s.Component(0)) | (u32(s.Component(1)) << 2) | (u32(s.Component(2)) << 4) | (u32(s.Component(3)) << 6)) << 4);
                in.words.Push(selection | (type << 12) | (dimension << 20) | (s.modifier ? 0x80000000u : 0u));
                if (s.modifier)
                    in.words.Push(0x1u | (u32(s.modifier) << 6));
                if (s.kind != Src::ThreadId)
                    in.words.Push(s.reg);
                if (s.kind == Src::Buffer)
                    in.words.Push(s.index);
            }

            void Simple(u32 opcode)
            {
                Instruction in(opcode);
                Finish(in);
            }

            // ---------------------------------------------------------------- registers and operands

            Src NewTemp(u32 count, bool isFloat = true)
            {
                Src s;
                s.reg = tempTop++;
                s.count = u8(count);
                s.isFloat = isFloat;
                tempMax = Max(tempMax, tempTop);
                return s;
            }

            static Src Imm(u32 bits, u32 count, bool isFloat)
            {
                Src s;
                s.kind = Src::Immediate;
                s.count = u8(count);
                s.isFloat = isFloat;
                for (u32& v : s.imm)
                    v = bits;
                return s;
            }
            static Src ImmF(f32 v, u32 count = 1) { return Imm(Bits(v), count, true); }
            static Src ImmI(u32 v, u32 count = 1) { return Imm(v, count, false); }

            static Src Neg(Src s)
            {
                s.modifier = s.modifier == 1 ? 0 : s.modifier == 2 ? 2 : 1;
                // |x| negated needs an instruction; callers only negate plain values.
                return s;
            }
            static Src Abs(Src s)
            {
                s.modifier = 2;
                return s;
            }

            static Src Pick(Src s, u32 a, u32 b = 0, u32 c = 0, u32 d = 0, u32 count = 1)
            {
                u8 picked[4] = {s.Component(a), s.Component(b), s.Component(c), s.Component(d)};
                for (u32 i = 0; i < 4; i++)
                    s.swizzle[i] = picked[i];
                s.count = u8(count);
                return s;
            }

            static Dst Whole(const Src& temp)
            {
                Dst d;
                d.reg = temp.reg;
                d.count = temp.count;
                return d;
            }

            // dest = op(sources), component wise, into a fresh register.
            Src Emit(u32 opcode, u32 count, const Src& a, bool isFloat = true)
            {
                Src d = NewTemp(count, isFloat);
                Instruction in(opcode);
                PutDst(in, Whole(d));
                PutSrc(in, a);
                Finish(in);
                return d;
            }
            Src Emit(u32 opcode, u32 count, const Src& a, const Src& b, bool isFloat = true)
            {
                Src d = NewTemp(count, isFloat);
                Instruction in(opcode);
                PutDst(in, Whole(d));
                PutSrc(in, a);
                PutSrc(in, b);
                Finish(in);
                return d;
            }
            Src Emit(u32 opcode, u32 count, const Src& a, const Src& b, const Src& c, bool isFloat = true)
            {
                Src d = NewTemp(count, isFloat);
                Instruction in(opcode);
                PutDst(in, Whole(d));
                PutSrc(in, a);
                PutSrc(in, b);
                PutSrc(in, c);
                Finish(in);
                return d;
            }

            // mov with the source components lined up with the destination components.
            void Store(const Dst& d, const Src& s)
            {
                Src arranged = s;
                arranged.count = 4;
                for (u32 i = 0; i < 4; i++)
                    arranged.swizzle[i] = s.Component(0);
                for (u32 e = 0; e < d.count; e++)
                    arranged.swizzle[d.comps[e]] = s.Component(e);
                Instruction in(kMov);
                PutDst(in, d);
                PutSrc(in, arranged);
                Finish(in);
            }

            // Conditions must be one component of a register.
            Src Scalar(const Src& s)
            {
                if (s.kind == Src::Temp && !s.modifier)
                    return s;
                Src t = NewTemp(1, s.isFloat);
                Store(Whole(t), s);
                return t;
            }

            void Conditional(u32 opcode, const Src& condition, bool nonZero = true)
            {
                Src c = Scalar(condition);
                Instruction in(opcode | (nonZero ? kTestNonZero : 0u));
                PutSrc(in, c, 0);
                Finish(in);
            }

            // ---------------------------------------------------------------- types and variables

            u32 RegisterCount(Type t) const
            {
                if (t.base == BaseType::Struct)
                {
                    u32 n = 0;
                    for (const StructField& f : m.structs[t.structIndex].fields)
                        n += RegisterCount(f.type);
                    return n;
                }
                return t.IsMatrix() ? t.cols : t.IsNumeric() ? 1u : 0u;
            }

            // The scalar and vector pieces a type is stored as, one register each; matrices by column.
            void Leaves(Type t, Array<Type>& out) const
            {
                if (t.base == BaseType::Struct)
                {
                    for (const StructField& f : m.structs[t.structIndex].fields)
                        Leaves(f.type, out);
                }
                else if (t.IsMatrix())
                {
                    for (u32 c = 0; c < t.cols; c++)
                        out.Push(Type::Vector(BaseType::Float, t.rows));
                }
                else if (t.IsNumeric())
                    out.Push(t);
            }

            Value Variable(Type t, u32 firstRegister) const
            {
                Value v;
                v.type = t;
                Array<Type> leaves;
                Leaves(t, leaves);
                for (usize i = 0; i < leaves.Size(); i++)
                {
                    Src s;
                    s.reg = firstRegister + u32(i);
                    s.count = leaves[i].rows;
                    s.isFloat = leaves[i].base == BaseType::Float;
                    v.parts.Push(s);
                }
                return v;
            }

            u32 Allocate(Type t)
            {
                u32 first = tempTop;
                tempTop += RegisterCount(t);
                tempMax = Max(tempMax, tempTop);
                return first;
            }

            void StoreValue(const Array<Dst>& places, const Value& v)
            {
                for (usize i = 0; i < places.Size() && i < v.parts.Size(); i++)
                    Store(places[i], v.parts[i]);
            }

            Array<Dst> Places(const Value& variable) const
            {
                Array<Dst> out;
                for (const Src& s : variable.parts)
                    out.Push(Whole(s));
                return out;
            }

            // Registers an assignment target refers to.
            bool Place(const Instance& in, ExprId id, Array<Dst>& out)
            {
                const Expr& e = in.fn->exprs[id];
                switch (e.kind)
                {
                case ExprKind::Local: out = Places(Variable(e.type, in.localReg[e.index])); return true;
                case ExprKind::Param: out = Places(Variable(e.type, in.paramReg[e.index])); return true;
                case ExprKind::Field:
                {
                    Array<Dst> whole;
                    if (!Place(in, e.a, whole))
                        return false;
                    Type owner = in.fn->exprs[e.a].type;
                    u32 first = 0;
                    for (u32 f = 0; f < e.index; f++)
                        first += RegisterCount(m.structs[owner.structIndex].fields[f].type);
                    for (u32 i = 0; i < RegisterCount(e.type); i++)
                        out.Push(whole[first + i]);
                    return true;
                }
                case ExprKind::Swizzle:
                {
                    Array<Dst> whole;
                    if (!Place(in, e.a, whole))
                        return false;
                    Dst d = whole[0];
                    for (u32 i = 0; i < e.swizzleCount; i++)
                        d.comps[i] = whole[0].comps[e.swizzle[i]];
                    d.count = e.swizzleCount;
                    out.Push(d);
                    return true;
                }
                case ExprKind::Index:
                {
                    Array<Dst> whole;
                    const Expr& index = in.fn->exprs[e.b];
                    if (index.kind != ExprKind::ConstInt)
                        return Fail("writing a vector or matrix element with a computed index is not supported by the Direct3D backend yet");
                    if (!Place(in, e.a, whole))
                        return false;
                    if (in.fn->exprs[e.a].type.IsMatrix())
                        out.Push(whole[usize(index.integer)]);
                    else
                    {
                        Dst d = whole[0];
                        d.comps[0] = whole[0].comps[index.integer & 3];
                        d.count = 1;
                        out.Push(d);
                    }
                    return true;
                }
                default: return Fail("an assignment target has no storage");
                }
            }

            // ---------------------------------------------------------------- storage buffers

            // One component operand: an immediate is written as a scalar.
            static void PutScalar(Instruction& in, const Src& s)
            {
                if (s.kind != Src::Immediate)
                    return PutSrc(in, s, 0);
                in.words.Push(0x00004001u);
                in.words.Push(s.imm[s.Component(0)]);
            }

            // Operand of a storage buffer: a view register (u) when writable, a resource register (t) otherwise.
            void PutStorage(Instruction& in, u32 global, u32 modeAndSwizzle)
            {
                const Global& g = m.globals[global];
                if (!storages.Find(global))
                    storages.Push(global);
                in.words.Push((g.writable ? 0x0011E000u : 0x00107000u) | modeAndSwizzle);
                in.words.Push(g.writable ? g.slot : 16u + g.slot);
            }

            // Structured buffer instructions carry the resource kind with the element size, and the return type.
            void PutStructuredInfo(Instruction& in, u32 global)
            {
                in.words[0] |= 0x80000000u;
                in.words.Push(0x80000002u | (12u << 6) | (SequentialSize(m, m.globals[global].element) << 11));
                in.words.Push(0x00199983u);
            }

            // Reads the value of a type that starts `offset` bytes into element `index`.
            Value BufferLoad(u32 global, const Src& index, u32 offset, Type type)
            {
                Value v;
                v.type = type;
                Array<Type> leaves;
                Leaves(type, leaves);
                for (const Type& leaf : leaves)
                {
                    Src d = NewTemp(leaf.rows, IsFloat(leaf));
                    Instruction in(kLdStructured);
                    PutStructuredInfo(in, global);
                    PutDst(in, Whole(d));
                    PutScalar(in, index);
                    PutScalar(in, ImmI(offset));
                    PutStorage(in, global, 0xE46u);
                    Finish(in);
                    v.parts.Push(d);
                    offset += 4u * leaf.rows;
                }
                return v;
            }

            void BufferStore(u32 global, const Src& index, u32 offset, const Value& v)
            {
                for (const Src& part : v.parts)
                {
                    Instruction in(kStoreStructured);
                    PutStorage(in, global, 0x2u | (((1u << part.count) - 1u) << 4));
                    PutScalar(in, index);
                    PutScalar(in, ImmI(offset));
                    PutSrc(in, part);
                    Finish(in);
                    offset += 4u * part.count;
                }
            }

            // An assignment target inside a buffer element: which buffer and element, where in it, and for a write to
            // some components of a vector which ones.
            struct BufferTarget
            {
                u32 global = 0, offset = 0;
                Src index;
                Type type;
                bool partial = false;
                u8 comps[4] = {0, 1, 2, 3};
                u8 count = 0;
            };

            bool IsStorage(const Instance& in, ExprId id) const
            {
                const Expr& e = in.fn->exprs[id];
                return e.kind == ExprKind::Global && m.globals[e.index].kind == GlobalKind::Storage;
            }

            // 1 when the target is in a buffer, 0 when it is not, -1 on an error.
            int FindBufferTarget(Instance& in, ExprId id, BufferTarget& out)
            {
                const Expr& e = in.fn->exprs[id];
                if (e.kind == ExprKind::Index && IsStorage(in, e.a))
                {
                    Value index;
                    if (!Eval(in, e.b, index))
                        return -1;
                    out.global = in.fn->exprs[e.a].index;
                    out.index = index.parts[0].kind == Src::Immediate ? index.parts[0] : Scalar(index.parts[0]);
                    out.type = e.type;
                    return 1;
                }
                if (e.kind != ExprKind::Field && e.kind != ExprKind::Swizzle && e.kind != ExprKind::Index)
                    return 0;
                int found = FindBufferTarget(in, e.a, out);
                if (found != 1)
                    return found;
                if (out.partial)
                    return Fail("this write into a buffer element is too deep for the Direct3D backend") ? 1 : -1;
                if (e.kind == ExprKind::Field)
                {
                    const StructType& owner = m.structs[out.type.structIndex];
                    for (u32 f = 0; f < e.index; f++)
                        out.offset += SequentialSize(m, owner.fields[f].type);
                    out.type = e.type;
                    return 1;
                }
                const Expr* index = e.kind == ExprKind::Index ? &in.fn->exprs[e.b] : nullptr;
                if (index && index->kind != ExprKind::ConstInt)
                    return Fail("writing a vector or matrix element of a buffer with a computed index is not supported by the Direct3D backend yet") ? 1 : -1;
                if (index && out.type.IsMatrix())
                {
                    out.offset += u32(index->integer) * 4u * out.type.rows;
                    out.type = Type::Vector(BaseType::Float, out.type.rows);
                    return 1;
                }
                out.partial = true;
                out.count = index ? 1 : e.swizzleCount;
                for (u32 i = 0; i < out.count; i++)
                    out.comps[i] = index ? u8(index->integer & 3) : e.swizzle[i];
                return 1;
            }

            // ---------------------------------------------------------------- expressions

            static bool IsFloat(Type t) { return t.base == BaseType::Float; }

            Value Single(Type t, const Src& s) const
            {
                Value v;
                v.type = t;
                v.parts.Push(s);
                return v;
            }

            // Truth values are all bits set or none, as comparison instructions produce them.
            Src Convert(const Src& s, Type from, Type to)
            {
                u32 n = to.rows;
                if (from.base == to.base)
                    return s;
                bool f = IsFloat(to);
                if (to.base == BaseType::Bool)
                    return IsFloat(from) ? Emit(kNe, n, s, ImmF(0.0f), false) : Emit(kIne, n, s, ImmI(0), false);
                if (from.base == BaseType::Bool)
                    return Emit(kAnd, n, s, f ? ImmF(1.0f) : ImmI(1), f);
                if (IsFloat(from))
                    return Emit(to.base == BaseType::Int ? kFtoi : kFtou, n, s, false);
                if (f)
                    return Emit(from.base == BaseType::Int ? kItof : kUtof, n, s);
                return s;
            }

            Src SignedDivide(u32 n, const Src& a, const Src& b, bool remainder)
            {
                // The format has unsigned division only: divide the magnitudes, then restore the sign.
                Src absA = Emit(kImax, n, a, Neg(a), false), absB = Emit(kImax, n, b, Neg(b), false);
                Src result = NewTemp(n, false);
                Instruction in(kUdiv);
                if (remainder)
                    PutNull(in);
                PutDst(in, Whole(result));
                if (!remainder)
                    PutNull(in);
                PutSrc(in, absA);
                PutSrc(in, absB);
                Finish(in);
                Src sign = remainder ? a : Emit(kXor, n, a, b, false);
                sign = Emit(kAnd, n, sign, ImmI(0x80000000u), false);
                return Emit(kMovc, n, sign, Emit(kIneg, n, result, false), result, false);
            }

            Src Arithmetic(Op op, Type operand, u32 n, const Src& a, const Src& b)
            {
                bool f = IsFloat(operand), u = operand.base == BaseType::UInt, bools = operand.base == BaseType::Bool;
                switch (op)
                {
                case Op::Add: return Emit(f ? kAdd : kIadd, n, a, b, f);
                case Op::Sub: return Emit(f ? kAdd : kIadd, n, a, Neg(b), f);
                case Op::Mul:
                {
                    if (f)
                        return Emit(kMul, n, a, b);
                    Src d = NewTemp(n, false);
                    Instruction in(kImul);
                    PutNull(in);
                    PutDst(in, Whole(d));
                    PutSrc(in, a);
                    PutSrc(in, b);
                    Finish(in);
                    return d;
                }
                case Op::Div:
                case Op::Rem:
                {
                    if (f && op == Op::Div)
                        return Emit(kDiv, n, a, b);
                    if (f)
                    {
                        // Remainder with the sign of the dividend: a - b * trunc(a / b).
                        Src q = Emit(kRoundZ, n, Emit(kDiv, n, a, b));
                        return Emit(kMad, n, Neg(q), b, a);
                    }
                    if (!u)
                        return SignedDivide(n, a, b, op == Op::Rem);
                    Src d = NewTemp(n, false);
                    Instruction in(kUdiv);
                    if (op == Op::Rem)
                        PutNull(in);
                    PutDst(in, Whole(d));
                    if (op == Op::Div)
                        PutNull(in);
                    PutSrc(in, a);
                    PutSrc(in, b);
                    Finish(in);
                    return d;
                }
                case Op::And: case Op::LogicalAnd: return Emit(kAnd, n, a, b, false);
                case Op::Or: case Op::LogicalOr: return Emit(kOr, n, a, b, false);
                case Op::Xor: return Emit(kXor, n, a, b, false);
                case Op::Shl: return Emit(kIshl, n, a, b, false);
                case Op::Shr: return Emit(u ? kUshr : kIshr, n, a, b, false);
                case Op::Eq: return Emit(f ? kEq : kIeq, n, a, b, false);
                case Op::Ne: return Emit(f ? kNe : kIne, n, a, b, false);
                // Greater and less-or-equal are the other two with swapped operands.
                case Op::Lt: return Emit(f ? kLt : u ? kUlt : kIlt, n, a, b, false);
                case Op::Ge: return Emit(f ? kGe : u ? kUge : kIge, n, a, b, false);
                case Op::Gt: return Emit(f ? kLt : u ? kUlt : kIlt, n, b, a, false);
                case Op::Le: return Emit(f ? kGe : u ? kUge : kIge, n, b, a, false);
                default: break;
                }
                (void)bools;
                return a;
            }

            Src Dot(u32 n, const Src& a, const Src& b)
            {
                if (n == 1)
                    return Emit(kMul, 1, a, b);
                return Emit(n == 2 ? kDp2 : n == 3 ? kDp3 : kDp4, 1, a, b);
            }

            // atan2 by a polynomial on the smaller ratio, then unfolded into the right octant.
            Src Atan2(u32 n, const Src& y, const Src& x)
            {
                Src ax = Emit(kMov, n, Abs(x)), ay = Emit(kMov, n, Abs(y));
                Src ratio = Emit(kDiv, n, Emit(kMin, n, ax, ay), Emit(kMax, n, ax, ay));
                Src s = Emit(kMul, n, ratio, ratio);
                Src p = Emit(kMad, n, s, ImmF(-0.0464964749f), ImmF(0.15931422f));
                p = Emit(kMad, n, p, s, ImmF(-0.327622764f));
                Src r = Emit(kMad, n, Emit(kMul, n, p, s), ratio, ratio);
                r = Emit(kMovc, n, Emit(kLt, n, ax, ay, false), Emit(kAdd, n, Neg(r), ImmF(1.57079637f)), r);
                r = Emit(kMovc, n, Emit(kLt, n, x, ImmF(0.0f), false), Emit(kAdd, n, Neg(r), ImmF(3.14159274f)), r);
                return Emit(kMovc, n, Emit(kLt, n, y, ImmF(0.0f), false), Emit(kMov, n, Neg(r)), r);
            }

            // acos by sqrt(1 - |x|) times a cubic, mirrored for negative arguments.
            Src Acos(u32 n, const Src& x)
            {
                Src a = Emit(kMov, n, Abs(x));
                Src p = Emit(kMad, n, a, ImmF(-0.0187293f), ImmF(0.0742610f));
                p = Emit(kMad, n, p, a, ImmF(-0.2121144f));
                p = Emit(kMad, n, p, a, ImmF(1.5707288f));
                Src r = Emit(kMul, n, p, Emit(kSqrt, n, Emit(kAdd, n, Neg(a), ImmF(1.0f))));
                return Emit(kMovc, n, Emit(kLt, n, x, ImmF(0.0f), false), Emit(kAdd, n, Neg(r), ImmF(3.14159274f)), r);
            }

            bool EvalIntrinsic(Instance& in, const Expr& e, Value& out)
            {
                Array<Value> args;
                for (ExprId id : e.args)
                {
                    const Expr& arg = in.fn->exprs[id];
                    if (arg.type.base == BaseType::Buffer)
                    {
                        if (!IsStorage(in, id))
                            return Fail("a buffer must be used directly from its static field");
                        Value v;
                        v.type = arg.type;
                        v.parts.Push(ImmI(arg.index));
                        args.Push(Move(v));
                        continue;
                    }
                    if (arg.type.base == BaseType::Texture2D)
                    {
                        // Textures are named by their slot; they have no value of their own.
                        if (arg.kind != ExprKind::Global)
                            return Fail("a texture must be used directly from its static field");
                        Value v;
                        v.type = arg.type;
                        v.parts.Push(ImmI(m.globals[arg.index].slot));
                        args.Push(Move(v));
                        continue;
                    }
                    if (!Eval(in, id, args.Emplace()))
                        return false;
                }
                Type rt = e.type;
                u32 n = rt.rows;
                auto arg = [&](usize i) -> const Src& { return args[i].parts[0]; };
                auto result = [&](const Src& s) {
                    out = Single(rt, s);
                    return true;
                };
                Type first = args.Empty() ? kVoid : args[0].type;
                u32 fn = first.rows;
                bool intType = first.IsInteger();

                switch (e.intrinsic)
                {
                case Intrinsic::Abs: return result(intType ? Emit(kImax, n, arg(0), Neg(arg(0)), false) : Emit(kMov, n, Abs(arg(0))));
                case Intrinsic::Sign:
                {
                    Src positive = Emit(kLt, n, ImmF(0.0f), arg(0), false), negative = Emit(kLt, n, arg(0), ImmF(0.0f), false);
                    return result(Emit(kItof, n, Emit(kIadd, n, negative, Neg(positive), false)));
                }
                case Intrinsic::Floor: return result(Emit(kRoundNi, n, arg(0)));
                case Intrinsic::Ceil: return result(Emit(kRoundPi, n, arg(0)));
                case Intrinsic::Round: return result(Emit(kRoundNe, n, arg(0)));
                case Intrinsic::Trunc: return result(Emit(kRoundZ, n, arg(0)));
                case Intrinsic::Frac: return result(Emit(kFrc, n, arg(0)));
                case Intrinsic::Sqrt: return result(Emit(kSqrt, n, arg(0)));
                case Intrinsic::Rsqrt: return result(Emit(kRsq, n, arg(0)));
                // The exp and log instructions work in base 2.
                case Intrinsic::Exp: return result(Emit(kExp, n, Emit(kMul, n, arg(0), ImmF(1.44269502f))));
                case Intrinsic::Exp2: return result(Emit(kExp, n, arg(0)));
                case Intrinsic::Log: return result(Emit(kMul, n, Emit(kLog, n, arg(0)), ImmF(0.693147182f)));
                case Intrinsic::Log2: return result(Emit(kLog, n, arg(0)));
                case Intrinsic::Sin: case Intrinsic::Cos: case Intrinsic::Tan:
                {
                    Src s = NewTemp(n), c = NewTemp(n);
                    Instruction ins(kSincos);
                    PutDst(ins, Whole(s));
                    PutDst(ins, Whole(c));
                    PutSrc(ins, arg(0));
                    Finish(ins);
                    return result(e.intrinsic == Intrinsic::Sin ? s : e.intrinsic == Intrinsic::Cos ? c : Emit(kDiv, n, s, c));
                }
                case Intrinsic::Asin: return result(Emit(kAdd, n, Neg(Acos(n, arg(0))), ImmF(1.57079637f)));
                case Intrinsic::Acos: return result(Acos(n, arg(0)));
                case Intrinsic::Atan: return result(Atan2(n, arg(0), ImmF(1.0f)));
                case Intrinsic::Atan2: return result(Atan2(n, arg(0), arg(1)));
                case Intrinsic::Saturate:
                {
                    Src d = NewTemp(n);
                    Instruction ins(kMov | kSaturate);
                    PutDst(ins, Whole(d));
                    PutSrc(ins, arg(0));
                    Finish(ins);
                    return result(d);
                }
                case Intrinsic::Radians: return result(Emit(kMul, n, arg(0), ImmF(0.0174532924f)));
                case Intrinsic::Degrees: return result(Emit(kMul, n, arg(0), ImmF(57.2957802f)));
                case Intrinsic::Pow: return result(Emit(kExp, n, Emit(kMul, n, Emit(kLog, n, arg(0)), arg(1))));
                case Intrinsic::Min: return result(Emit(IsFloat(first) ? kMin : first.base == BaseType::Int ? kImin : kUmin, n, arg(0), arg(1), IsFloat(first)));
                case Intrinsic::Max: return result(Emit(IsFloat(first) ? kMax : first.base == BaseType::Int ? kImax : kUmax, n, arg(0), arg(1), IsFloat(first)));
                case Intrinsic::Clamp:
                {
                    bool f = IsFloat(first), i = first.base == BaseType::Int;
                    Src low = Emit(f ? kMax : i ? kImax : kUmax, n, arg(0), arg(1), f);
                    return result(Emit(f ? kMin : i ? kImin : kUmin, n, low, arg(2), f));
                }
                case Intrinsic::Step: return result(Emit(kAnd, n, Emit(kGe, n, arg(1), arg(0), false), ImmF(1.0f)));
                case Intrinsic::Fmod: return result(Arithmetic(Op::Rem, first, n, arg(0), arg(1)));
                case Intrinsic::Lerp: return result(Emit(kMad, n, Emit(kAdd, n, arg(1), Neg(arg(0))), arg(2), arg(0)));
                case Intrinsic::Smoothstep:
                {
                    Src width = Emit(kAdd, n, arg(1), Neg(arg(0)));
                    Src t = NewTemp(n);
                    Instruction ins(kDiv | kSaturate);
                    PutDst(ins, Whole(t));
                    PutSrc(ins, Emit(kAdd, n, arg(2), Neg(arg(0))));
                    PutSrc(ins, width);
                    Finish(ins);
                    Src curve = Emit(kMad, n, t, ImmF(-2.0f), ImmF(3.0f));
                    return result(Emit(kMul, n, Emit(kMul, n, t, t), curve));
                }
                case Intrinsic::Dot: return result(Dot(fn, arg(0), arg(1)));
                case Intrinsic::Length: return result(Emit(kSqrt, 1, Dot(fn, arg(0), arg(0))));
                case Intrinsic::Distance:
                {
                    Src d = Emit(kAdd, fn, arg(0), Neg(arg(1)));
                    return result(Emit(kSqrt, 1, Dot(fn, d, d)));
                }
                case Intrinsic::Normalize: return result(Emit(kMul, n, arg(0), Emit(kRsq, 1, Dot(fn, arg(0), arg(0)))));
                case Intrinsic::Reflect:
                {
                    Src d = Dot(fn, arg(1), arg(0));
                    Src scale = Emit(kAdd, 1, d, d);
                    return result(Emit(kMad, n, Neg(scale), arg(1), arg(0)));
                }
                case Intrinsic::Refract:
                {
                    Src d = Dot(fn, arg(1), arg(0));
                    Src eta = arg(2);
                    Src k = Emit(kMad, 1, Neg(d), d, ImmF(1.0f));
                    k = Emit(kMad, 1, Neg(Emit(kMul, 1, eta, eta)), k, ImmF(1.0f));
                    Src factor = Emit(kMad, 1, eta, d, Emit(kSqrt, 1, k));
                    Src bent = Emit(kMad, n, Neg(factor), arg(1), Emit(kMul, n, arg(0), eta));
                    return result(Emit(kMovc, n, Emit(kLt, 1, k, ImmF(0.0f), false), ImmF(0.0f), bent));
                }
                case Intrinsic::Cross:
                {
                    Src a = arg(0), b = arg(1);
                    Src left = Emit(kMul, 3, Pick(a, 1, 2, 0, 0, 3), Pick(b, 2, 0, 1, 0, 3));
                    return result(Emit(kMad, 3, Neg(Pick(a, 2, 0, 1, 0, 3)), Pick(b, 1, 2, 0, 0, 3), left));
                }
                case Intrinsic::Ddx: return result(Emit(kDerivRtx, n, arg(0)));
                case Intrinsic::Ddy: return result(Emit(kDerivRty, n, arg(0)));
                case Intrinsic::Fwidth: return result(Emit(kAdd, n, Abs(Emit(kDerivRtx, n, arg(0))), Abs(Emit(kDerivRty, n, arg(0)))));
                case Intrinsic::Select: return result(Emit(kMovc, n, arg(0), arg(1), arg(2), IsFloat(rt)));
                case Intrinsic::Any: case Intrinsic::All:
                {
                    Src acc = Pick(arg(0), 0);
                    for (u32 c = 1; c < fn; c++)
                        acc = Emit(e.intrinsic == Intrinsic::Any ? kOr : kAnd, 1, acc, Pick(arg(0), c), false);
                    return result(acc);
                }
                case Intrinsic::Mul:
                {
                    // Matrices are stored by column: M * v is the sum of the columns scaled by the components of v.
                    const Value& matrix = args[0];
                    u32 size = matrix.type.cols;
                    auto transform = [&](const Src& v) {
                        Src acc = Emit(kMul, size, matrix.parts[0], Pick(v, 0));
                        for (u32 c = 1; c < size; c++)
                            acc = Emit(kMad, size, matrix.parts[c], Pick(v, c), acc);
                        return acc;
                    };
                    out.type = rt;
                    for (const Src& column : args[1].parts)
                        out.parts.Push(transform(column));
                    return true;
                }
                case Intrinsic::Transpose:
                {
                    u32 size = first.cols;
                    out.type = rt;
                    for (u32 column = 0; column < size; column++)
                    {
                        Src t = NewTemp(size);
                        for (u32 row = 0; row < size; row++)
                        {
                            Dst d = Whole(t);
                            d.comps[0] = u8(row);
                            d.count = 1;
                            Store(d, Pick(args[0].parts[row], column));
                        }
                        out.parts.Push(t);
                    }
                    return true;
                }
                case Intrinsic::TextureSample: case Intrinsic::TextureSampleLevel:
                {
                    u32 slot = arg(0).imm[0];
                    UseTexture(slot);
                    Src d = NewTemp(4);
                    Instruction ins(e.intrinsic == Intrinsic::TextureSample ? kSample : kSampleL);
                    PutDst(ins, Whole(d));
                    PutSrc(ins, arg(1));
                    ins.words.Push(0x00107E46u);
                    ins.words.Push(slot);
                    ins.words.Push(0x00106000u);
                    ins.words.Push(slot);
                    if (e.intrinsic == Intrinsic::TextureSampleLevel)
                        PutSrc(ins, arg(2), 0);
                    Finish(ins);
                    return result(d);
                }
                case Intrinsic::TextureLoad:
                {
                    u32 slot = arg(0).imm[0];
                    UseTexture(slot);
                    // The coordinate is (x, y, mip, unused).
                    Src coord = NewTemp(4, false);
                    Dst xy = Whole(coord);
                    xy.count = 2;
                    Store(xy, arg(1));
                    Dst zw = Whole(coord);
                    zw.comps[0] = 2;
                    zw.comps[1] = 3;
                    zw.count = 2;
                    Store(zw, Pick(arg(2), 0, 0, 0, 0, 2));
                    Src d = NewTemp(4);
                    Instruction ins(kLd);
                    PutDst(ins, Whole(d));
                    PutSrc(ins, coord);
                    ins.words.Push(0x00107E46u);
                    ins.words.Push(slot);
                    Finish(ins);
                    return result(d);
                }
                case Intrinsic::BufferLength:
                {
                    u32 global = arg(0).imm[0];
                    Src d = NewTemp(1, false);
                    Instruction ins(kBufinfo);
                    PutStructuredInfo(ins, global);
                    PutDst(ins, Whole(d));
                    PutStorage(ins, global, 0x006u);
                    Finish(ins);
                    return result(d);
                }
                case Intrinsic::TextureSize:
                {
                    u32 slot = arg(0).imm[0];
                    UseTexture(slot);
                    Src d = NewTemp(4, false);
                    // Control value 2 asks for unsigned integers.
                    Instruction ins(kResinfo | (2u << 11));
                    PutDst(ins, Whole(d));
                    PutSrc(ins, ImmI(0), 0);
                    ins.words.Push(0x00107E46u);
                    ins.words.Push(slot);
                    Finish(ins);
                    d.count = 2;
                    return result(d);
                }
                default: return Fail(Format("the function {} is not supported by the Direct3D backend yet", IntrinsicName(e.intrinsic)));
                }
            }

            void UseTexture(u32 slot)
            {
                if (!textures.Find(slot))
                    textures.Push(slot);
            }

            bool Eval(Instance& in, ExprId id, Value& out)
            {
                const Expr& e = in.fn->exprs[id];
                out.type = e.type;
                out.parts.Clear();
                u32 n = e.type.rows;
                switch (e.kind)
                {
                case ExprKind::ConstFloat: out.parts.Push(ImmF(f32(e.number))); return true;
                case ExprKind::ConstInt: out.parts.Push(ImmI(u32(e.integer))); return true;
                case ExprKind::ConstBool: out.parts.Push(ImmI(e.integer ? 0xFFFFFFFFu : 0u)); return true;
                case ExprKind::Local: out = Variable(e.type, in.localReg[e.index]); return true;
                case ExprKind::Param: out = Variable(e.type, in.paramReg[e.index]); return true;
                case ExprKind::Global:
                {
                    const Global& g = m.globals[e.index];
                    if (g.kind != GlobalKind::Uniform)
                        return Fail(Format("{} can only be used through its methods", g.name));
                    Array<Type> leaves;
                    Leaves(e.type, leaves);
                    u32 offset = 0;
                    for (const Type& leaf : leaves)
                    {
                        Src s;
                        s.kind = Src::Buffer;
                        s.reg = g.slot;
                        s.index = offset / 16;
                        s.count = leaf.rows;
                        s.isFloat = IsFloat(leaf);
                        for (u32 i = 0; i < 4; i++)
                            s.swizzle[i] = u8(Min(offset % 16 / 4 + i, 3u));
                        out.parts.Push(s);
                        offset += 4u * leaf.rows;
                    }
                    u32 size = u32(AlignUp(offset, 16)) / 16;
                    bool known = false;
                    for (Resource& r : buffers)
                        if (r.slot == g.slot)
                        {
                            r.size = Max(r.size, size);
                            known = true;
                        }
                    if (!known)
                        buffers.Push({g.slot, size});
                    return true;
                }
                case ExprKind::Field:
                {
                    Value whole;
                    if (!Eval(in, e.a, whole))
                        return false;
                    u32 first = 0;
                    for (u32 f = 0; f < e.index; f++)
                        first += RegisterCount(m.structs[whole.type.structIndex].fields[f].type);
                    for (u32 i = 0; i < RegisterCount(e.type); i++)
                        out.parts.Push(whole.parts[first + i]);
                    return true;
                }
                case ExprKind::Swizzle:
                {
                    Value whole;
                    if (!Eval(in, e.a, whole))
                        return false;
                    out.parts.Push(Pick(whole.parts[0], e.swizzle[0], e.swizzle[1], e.swizzle[2], e.swizzle[3], e.swizzleCount));
                    return true;
                }
                case ExprKind::Index:
                {
                    Value whole, index;
                    if (IsStorage(in, e.a))
                    {
                        if (!Eval(in, e.b, index))
                            return false;
                        out = BufferLoad(in.fn->exprs[e.a].index, index.parts[0].kind == Src::Immediate ? index.parts[0] : Scalar(index.parts[0]), 0, e.type);
                        return true;
                    }
                    if (!Eval(in, e.a, whole) || !Eval(in, e.b, index))
                        return false;
                    bool matrix = whole.type.IsMatrix();
                    u32 elements = matrix ? whole.type.cols : whole.type.rows;
                    auto element = [&](u32 i) { return matrix ? whole.parts[i] : Pick(whole.parts[0], i); };
                    const Src& idx = index.parts[0];
                    if (idx.kind == Src::Immediate)
                    {
                        out.parts.Push(element(Min(idx.imm[0], elements - 1)));
                        return true;
                    }
                    // A computed index selects by comparing with each position.
                    Src picked = element(0);
                    for (u32 i = 1; i < elements; i++)
                        picked = Emit(kMovc, n, Emit(kIeq, 1, idx, ImmI(i), false), element(i), picked, IsFloat(e.type));
                    out.parts.Push(picked);
                    return true;
                }
                case ExprKind::Unary:
                {
                    Value a;
                    if (!Eval(in, e.a, a))
                        return false;
                    for (const Src& s : a.parts)
                    {
                        if (e.op == Op::Neg)
                            out.parts.Push(IsFloat(a.type) ? Emit(kMov, s.count, Neg(s)) : Emit(kIneg, s.count, s, false));
                        else
                            out.parts.Push(Emit(kNot, s.count, s, false));
                    }
                    return true;
                }
                case ExprKind::Binary:
                {
                    Value a, b;
                    if (!Eval(in, e.a, a) || !Eval(in, e.b, b))
                        return false;
                    // Matrix with scalar or matrix with matrix: column by column.
                    usize parts = Max(a.parts.Size(), b.parts.Size());
                    for (usize i = 0; i < parts; i++)
                    {
                        const Src& x = a.parts[Min(i, a.parts.Size() - 1)];
                        const Src& y = b.parts[Min(i, b.parts.Size() - 1)];
                        out.parts.Push(Arithmetic(e.op, a.type, Max<u32>(x.count, y.count), x, y));
                    }
                    return true;
                }
                case ExprKind::Cast:
                {
                    Value a;
                    if (!Eval(in, e.a, a))
                        return false;
                    out.parts.Push(Convert(a.parts[0], a.type, e.type));
                    return true;
                }
                case ExprKind::Construct:
                {
                    Array<Type> leaves;
                    Leaves(e.type, leaves);
                    if (e.args.Empty())
                    {
                        for (const Type& leaf : leaves)
                            out.parts.Push(Imm(0, leaf.rows, IsFloat(leaf)));
                        return true;
                    }
                    Array<Value> args;
                    for (ExprId arg : e.args)
                        if (!Eval(in, arg, args.Emplace()))
                            return false;
                    if (e.type.IsMatrix())
                    {
                        if (args.Size() == 1 && args[0].type.IsScalar())
                        {
                            // One scalar is the diagonal.
                            for (u32 c = 0; c < e.type.cols; c++)
                            {
                                Src column = NewTemp(e.type.rows);
                                Store(Whole(column), ImmF(0.0f));
                                Dst d = Whole(column);
                                d.comps[0] = u8(c);
                                d.count = 1;
                                Store(d, args[0].parts[0]);
                                out.parts.Push(column);
                            }
                            return true;
                        }
                        for (const Value& column : args)
                            out.parts.Push(column.parts[0]);
                        return true;
                    }
                    if (args.Size() == 1 && args[0].parts[0].count == 1)
                    {
                        out.parts.Push(Pick(args[0].parts[0], 0, 0, 0, 0, n));
                        return true;
                    }
                    // Components come from the arguments in order, each a scalar or a vector.
                    Src built = NewTemp(n, IsFloat(e.type));
                    u32 at = 0;
                    for (const Value& a : args)
                    {
                        const Src& s = a.parts[0];
                        Dst d = Whole(built);
                        d.count = s.count;
                        for (u32 i = 0; i < s.count && at + i < 4; i++)
                            d.comps[i] = u8(at + i);
                        Store(d, s);
                        at += s.count;
                    }
                    out.parts.Push(built);
                    return true;
                }
                case ExprKind::Call: return Call(in, e, out);
                case ExprKind::Intrinsic: return EvalIntrinsic(in, e, out);
                }
                return Fail("unknown expression");
            }

            // ---------------------------------------------------------------- calls and statements

            static bool ContainsLoop(const Array<Stmt>& stmts)
            {
                for (const Stmt& s : stmts)
                    if (s.kind == StmtKind::Loop || ContainsLoop(s.body) || ContainsLoop(s.elseBody))
                        return true;
                return false;
            }

            void AllocateVariables(Instance& inst)
            {
                for (const helix::shadercompiler::Variable& v : inst.fn->params)
                    inst.paramReg.Push(Allocate(v.type));
                for (const helix::shadercompiler::Variable& v : inst.fn->locals)
                    inst.localReg.Push(Allocate(v.type));
            }

            // The bytecode has no functions: the body is copied in, wrapped in a loop that runs once so that a return is
            // a break. A return inside a loop of the callee sets a flag that every enclosing loop checks.
            bool Call(Instance& caller, const Expr& e, Value& out)
            {
                const Function& callee = m.functions[e.index];
                Array<Value> args;
                for (usize i = 0; i < e.args.Size(); i++)
                {
                    args.Emplace();
                    if (callee.params[i].mode != ParamMode::Out && !Eval(caller, e.args[i], args.Back()))
                        return false;
                }
                Instance inst;
                inst.fn = &callee;
                AllocateVariables(inst);
                inst.retReg = Allocate(callee.returnType);
                if (hasLoop[e.index])
                {
                    inst.flagReg = i32(NewTemp(1, false).reg);
                    Dst flag;
                    flag.reg = u32(inst.flagReg);
                    Store(flag, ImmI(0));
                }
                for (usize i = 0; i < e.args.Size(); i++)
                    if (callee.params[i].mode != ParamMode::Out)
                        StoreValue(Places(Variable(callee.params[i].type, inst.paramReg[i])), args[i]);
                Simple(kLoop);
                if (!Statements(inst, callee.body))
                    return false;
                Simple(kBreak);
                Simple(kEndloop);
                for (usize i = 0; i < e.args.Size(); i++)
                {
                    if (callee.params[i].mode == ParamMode::In)
                        continue;
                    Array<Dst> places;
                    if (!Place(caller, e.args[i], places))
                        return false;
                    StoreValue(places, Variable(callee.params[i].type, inst.paramReg[i]));
                }
                out = Variable(callee.returnType, inst.retReg);
                return true;
            }

            bool Statements(Instance& in, const Array<Stmt>& stmts)
            {
                for (const Stmt& s : stmts)
                {
                    // Registers of intermediate values are free again after the statement.
                    u32 mark = tempTop;
                    if (!Statement(in, s))
                        return false;
                    tempTop = mark;
                }
                return true;
            }

            static bool IsOnly(const Array<Stmt>& stmts, StmtKind kind) { return stmts.Size() == 1 && stmts[0].kind == kind; }

            // Statements at the end of a loop body: a continue there does nothing, and a branch that only breaks becomes
            // a conditional break, the form drivers see from other compilers.
            bool LoopTail(Instance& in, const Array<Stmt>& stmts)
            {
                if (stmts.Empty())
                    return true;
                for (usize i = 0; i + 1 < stmts.Size(); i++)
                {
                    u32 mark = tempTop;
                    if (!Statement(in, stmts[i]))
                        return false;
                    tempTop = mark;
                }
                const Stmt& last = stmts.Back();
                if (last.kind == StmtKind::Continue)
                    return true;
                if (last.kind != StmtKind::If)
                    return Statement(in, last);
                u32 mark = tempTop;
                Value value;
                if (!Eval(in, last.a, value))
                    return false;
                const Src& condition = value.parts[0];
                bool ok;
                if (condition.kind == Src::Immediate)
                    ok = LoopTail(in, condition.imm[0] ? last.body : last.elseBody);
                else if (IsOnly(last.elseBody, StmtKind::Break) || IsOnly(last.body, StmtKind::Break))
                {
                    bool breakWhenTrue = IsOnly(last.body, StmtKind::Break);
                    Conditional(kBreakc, condition, breakWhenTrue);
                    tempTop = mark;
                    ok = LoopTail(in, breakWhenTrue ? last.elseBody : last.body);
                }
                else
                {
                    Conditional(kIf, condition, !last.body.Empty());
                    ok = LoopTail(in, last.body.Empty() ? last.elseBody : last.body);
                    if (ok && !last.body.Empty() && !last.elseBody.Empty())
                    {
                        Simple(kElse);
                        ok = LoopTail(in, last.elseBody);
                    }
                    Simple(kEndif);
                }
                tempTop = mark;
                return ok;
            }

            bool Statement(Instance& in, const Stmt& s)
            {
                Value value;
                switch (s.kind)
                {
                case StmtKind::Assign:
                {
                    Array<Dst> places;
                    if (!Eval(in, s.b, value))
                        return false;
                    BufferTarget buffer;
                    if (int found = FindBufferTarget(in, s.a, buffer))
                    {
                        if (found < 0)
                            return false;
                        if (buffer.partial)
                        {
                            // Some components of a vector: read it, change them, write it back.
                            Value whole = BufferLoad(buffer.global, buffer.index, buffer.offset, buffer.type);
                            Dst d = Whole(whole.parts[0]);
                            d.count = buffer.count;
                            for (u32 i = 0; i < buffer.count; i++)
                                d.comps[i] = buffer.comps[i];
                            Store(d, value.parts[0]);
                            value = Move(whole);
                        }
                        BufferStore(buffer.global, buffer.index, buffer.offset, value);
                        return true;
                    }
                    if (!Place(in, s.a, places))
                        return false;
                    if (places.Size() > 1)
                    {
                        // A multi register value may read what an earlier store of the same assignment wrote.
                        for (Src& part : value.parts)
                            if (part.kind == Src::Temp)
                                part = Emit(kMov, part.count, part, part.isFloat);
                    }
                    StoreValue(places, value);
                    return true;
                }
                case StmtKind::Evaluate: return Eval(in, s.a, value);
                case StmtKind::Return:
                    if (in.entry)
                    {
                        if (s.a)
                        {
                            if (!Eval(in, s.a, value))
                                return false;
                            StoreValue(entryOutputs, value);
                        }
                        Simple(kRet);
                        return true;
                    }
                    if (s.a)
                    {
                        if (!Eval(in, s.a, value))
                            return false;
                        StoreValue(Places(Variable(in.fn->returnType, in.retReg)), value);
                    }
                    if (in.loopDepth > 0)
                    {
                        Dst flag;
                        flag.reg = u32(in.flagReg);
                        Store(flag, ImmI(0xFFFFFFFFu));
                    }
                    Simple(kBreak);
                    return true;
                case StmtKind::Discard:
                {
                    Src all = NewTemp(1, false);
                    Store(Whole(all), ImmI(0xFFFFFFFFu));
                    Conditional(kDiscard, all);
                    return true;
                }
                case StmtKind::Break: Simple(kBreak); return true;
                case StmtKind::Continue: Simple(kContinue); return true;
                case StmtKind::If:
                {
                    if (!Eval(in, s.a, value))
                        return false;
                    const Src& condition = value.parts[0];
                    if (condition.kind == Src::Immediate)
                        return Statements(in, condition.imm[0] ? s.body : s.elseBody);
                    // An empty first branch tests for zero instead.
                    Conditional(kIf, condition, !s.body.Empty());
                    if (!Statements(in, s.body.Empty() ? s.elseBody : s.body))
                        return false;
                    if (!s.body.Empty() && !s.elseBody.Empty())
                    {
                        Simple(kElse);
                        if (!Statements(in, s.elseBody))
                            return false;
                    }
                    Simple(kEndif);
                    return true;
                }
                case StmtKind::Loop:
                {
                    Simple(kLoop);
                    in.loopDepth++;
                    if (!LoopTail(in, s.body))
                        return false;
                    in.loopDepth--;
                    Simple(kEndloop);
                    if (!in.entry && in.flagReg >= 0)
                    {
                        // The callee returned from inside the loop: leave the next enclosing loop too.
                        Src flag;
                        flag.reg = u32(in.flagReg);
                        flag.isFloat = false;
                        Conditional(kBreakc, flag);
                    }
                    return true;
                }
                }
                return Fail("unknown statement");
            }

            // ---------------------------------------------------------------- stage interface and container

            Array<SignatureElement> inputs, outputs;
            Array<u32> declarations;

            void Declare(std::initializer_list<u32> words)
            {
                u32 first = u32(declarations.Size());
                for (u32 w : words)
                    declarations.Push(w);
                declarations[first] |= u32(words.size()) << 24;
            }

            static u32 ComponentType(Type t) { return t.base == BaseType::Float ? 3u : t.base == BaseType::Int ? 2u : 1u; }

            bool Input(Semantic semantic, u32 index, Type type, bool flat, const Dst& target, u32& nextSystemRegister, StringView what)
            {
                if (m.stage == Stage::Compute)
                {
                    if (semantic != Semantic::DispatchThreadId || type.rows != 3 || !type.IsInteger())
                        return Fail(Format("{}: a compute shader takes a uint3 or int3 [DispatchThreadId]", what));
                    Src id;
                    id.kind = Src::ThreadId;
                    id.count = 3;
                    id.isFloat = false;
                    threadId = true;
                    Store(target, id);
                    return true;
                }
                bool vertex = m.stage == Stage::Vertex;
                u32 mask = (1u << type.rows) - 1;
                Src s;
                s.kind = Src::Input;
                s.count = type.rows;
                s.isFloat = IsFloat(type);
                if (semantic == Semantic::Location)
                {
                    s.reg = vertex ? index : index + 1;
                    inputs.Push({"TEXCOORD", index, 0, ComponentType(type), s.reg, mask});
                    if (vertex)
                        Declare({kDclInput, 0x00101002u | (mask << 4), s.reg});
                    else
                        Declare({kDclInputPs | ((flat || !IsFloat(type) ? 1u : 2u) << 11), 0x00101002u | (mask << 4), s.reg});
                }
                else if (semantic == Semantic::Position && !vertex)
                {
                    s.reg = 0;
                    inputs.Push({"SV_Position", 0, 1, 3, 0, 0xF});
                    Declare({kDclInputPsSiv | (4u << 11), 0x001010F2u, 0, 1});
                }
                else if ((semantic == Semantic::VertexId || semantic == Semantic::InstanceId) && vertex)
                {
                    bool vertexId = semantic == Semantic::VertexId;
                    s.reg = nextSystemRegister++;
                    inputs.Push({vertexId ? "SV_VertexID" : "SV_InstanceID", 0, vertexId ? 6u : 8u, 1, s.reg, 1});
                    Declare({kDclInputSgv, 0x00101012u, s.reg, vertexId ? 6u : 8u});
                }
                else
                    return Fail(Format("{} has a stage attribute the Direct3D backend does not support here", what));
                Store(target, s);
                return true;
            }

            bool Output(Semantic semantic, u32 index, Type type, StringView what)
            {
                bool vertex = m.stage == Stage::Vertex;
                u32 mask = (1u << type.rows) - 1;
                Dst d;
                d.output = true;
                d.count = type.rows;
                if (semantic == Semantic::Position && vertex)
                {
                    d.reg = 0;
                    outputs.Push({"SV_Position", 0, 1, 3, 0, 0xF});
                    Declare({kDclOutputSiv, 0x001020F2u, 0, 1});
                }
                else if (semantic == Semantic::Location && vertex)
                {
                    d.reg = index + 1;
                    outputs.Push({"TEXCOORD", index, 0, ComponentType(type), d.reg, mask});
                    Declare({kDclOutput, 0x00102002u | (mask << 4), d.reg});
                }
                else if (semantic == Semantic::Target && !vertex)
                {
                    d.reg = index;
                    outputs.Push({"SV_Target", index, 0, ComponentType(type), d.reg, mask});
                    Declare({kDclOutput, 0x00102002u | (mask << 4), d.reg});
                }
                else
                    return Fail(Format("{} has a stage attribute the Direct3D backend does not support here", what));
                entryOutputs.Push(d);
                return true;
            }

            static void Append(Array<u8>& out, const void* data, usize size) { out.Append(static_cast<const u8*>(data), size); }
            static void AppendU32(Array<u8>& out, u32 v) { Append(out, &v, 4); }

            static Array<u8> Signature(Array<SignatureElement>& elements, bool isOutput)
            {
                // Elements are listed by register.
                for (usize i = 1; i < elements.Size(); i++)
                    for (usize j = i; j > 0 && elements[j - 1].reg > elements[j].reg; j--)
                        Swap(elements[j - 1], elements[j]);
                Array<u8> out;
                AppendU32(out, u32(elements.Size()));
                AppendU32(out, 8);
                u32 nameOffset = 8 + 24 * u32(elements.Size());
                for (const SignatureElement& e : elements)
                {
                    AppendU32(out, nameOffset);
                    AppendU32(out, e.index);
                    AppendU32(out, e.systemValue);
                    AppendU32(out, e.componentType);
                    AppendU32(out, e.reg);
                    // The last byte pair: the mask, then the components read (inputs) or never written (outputs).
                    AppendU32(out, e.mask | ((isOutput ? ~e.mask & 0xFu : e.unused ? 0u : e.mask) << 8));
                    nameOffset += u32(std::strlen(e.name)) + 1;
                }
                for (const SignatureElement& e : elements)
                    Append(out, e.name, std::strlen(e.name) + 1);
                while (out.Size() % 4)
                    out.Push(0xAB);
                return out;
            }

            // MD5 rounds with the container's own padding: the bit count leads the last block and (bits >> 2) | 1 ends it.
            static void Checksum(const u8* data, usize size, u32 state[4])
            {
                for (u32 i = 0; i < 4; i++)
                    state[i] = Md5::kInitialState[i];
                usize full = size / 64 * 64;
                for (usize i = 0; i < full; i += 64)
                    Md5::ProcessBlock(state, data + i);
                usize rest = size - full;
                u32 bits = u32(size * 8), tail = (bits >> 2) | 1;
                u8 block[64] = {};
                if (rest >= 56)
                {
                    std::memcpy(block, data + full, rest);
                    block[rest] = 0x80;
                    Md5::ProcessBlock(state, block);
                    std::memset(block, 0, 64);
                    std::memcpy(block, &bits, 4);
                }
                else
                {
                    std::memcpy(block, &bits, 4);
                    std::memcpy(block + 4, data + full, rest);
                    block[4 + rest] = 0x80;
                }
                std::memcpy(block + 60, &tail, 4);
                Md5::ProcessBlock(state, block);
            }

            bool Run(DxbcShader& shader)
            {
                for (const Global& g : m.globals)
                {
                    if (g.kind == GlobalKind::Uniform && (g.type.base != BaseType::Struct || !CheckUniformLayout(m, g.type, g.name, 0, 0, error)))
                        return Fail(Format("uniform {} must be a struct", g.name));
                    if (g.kind == GlobalKind::Storage && !CheckBufferLayout(m, g.element, g.name, error))
                        return false;
                }
                for (const Function& f : m.functions)
                    hasLoop.Push(ContainsLoop(f.body) ? 1 : 0);

                Instance entry;
                entry.fn = &m.functions[m.entry];
                entry.entry = true;
                AllocateVariables(entry);
                const Function& fn = *entry.fn;

                // Inputs are copied into the parameters, outputs are written at every return.
                u32 systemRegister = 0;
                for (const helix::shadercompiler::Variable& p : fn.params)
                {
                    if (p.type.base == BaseType::Struct)
                    {
                        for (const StructField& f : m.structs[p.type.structIndex].fields)
                            if (f.semantic == Semantic::Location)
                                systemRegister = Max(systemRegister, f.semanticIndex + 1);
                    }
                    else if (p.semantic == Semantic::Location)
                        systemRegister = Max(systemRegister, p.semanticIndex + 1);
                }
                for (usize i = 0; i < fn.params.Size(); i++)
                {
                    const helix::shadercompiler::Variable& p = fn.params[i];
                    if (p.mode != ParamMode::In)
                        return Fail(Format("entry point parameter {} is ref or out; return the outputs instead", p.name));
                    Array<Dst> places = Places(Variable(p.type, entry.paramReg[i]));
                    if (p.type.base == BaseType::Struct)
                    {
                        u32 part = 0;
                        for (const StructField& f : m.structs[p.type.structIndex].fields)
                        {
                            if (RegisterCount(f.type) != 1)
                                return Fail(Format("field {} of parameter {} must be a scalar or vector", f.name, p.name));
                            if (!Input(f.semantic, f.semanticIndex, f.type, f.flat, places[part++], systemRegister, Format("field {} of parameter {}", f.name, p.name)))
                                return false;
                        }
                    }
                    else if (!Input(p.semantic, p.semanticIndex, p.type, p.flat, places[0], systemRegister, Format("parameter {}", p.name)))
                        return false;
                }
                // The fragment signature lists the position the vertex stage always writes, read or not.
                bool position = false;
                for (const SignatureElement& e : inputs)
                    position = position || e.systemValue == 1;
                if (m.stage == Stage::Fragment && !position)
                    inputs.Push({"SV_Position", 0, 1, 3, 0, 0xF, true});
                Type r = fn.returnType;
                if (r.base == BaseType::Struct)
                {
                    for (const StructField& f : m.structs[r.structIndex].fields)
                    {
                        if (RegisterCount(f.type) != 1)
                            return Fail(Format("field {} of the return value must be a scalar or vector", f.name));
                        if (!Output(f.semantic, f.semanticIndex, f.type, Format("field {} of the return value", f.name)))
                            return false;
                    }
                }
                else if (!r.IsVoid() && !Output(m.returnSemantic, m.returnSemanticIndex, r, "the return value"))
                    return false;

                if (!Statements(entry, fn.body))
                    return false;
                // Nothing may follow the last ret; the software rasterizer loses the device on it.
                if (lastOpcode != kRet)
                    Simple(kRet);

                // Shader chunk: version, length, declarations, code.
                Array<u32> body;
                for (const Resource& b : buffers)
                {
                    u32 words[] = {kDclConstantBuffer | (4u << 24), 0x00208E46u, b.slot, b.size};
                    body.Append(words, 4);
                }
                for (u32 slot : textures)
                {
                    u32 words[] = {kDclSampler | (3u << 24), 0x00106000u, slot, kDclResource | (3u << 11) | (4u << 24), 0x00107000u, slot, 0x00005555u};
                    body.Append(words, 7);
                }
                for (u32 global : storages)
                {
                    const Global& g = m.globals[global];
                    u32 stride = SequentialSize(m, g.element);
                    u32 words[] = {(g.writable ? kDclUavStructured : kDclResourceStructured) | (4u << 24), g.writable ? 0x0011E000u : 0x00107000u,
                                   g.writable ? g.slot : 16u + g.slot, stride};
                    body.Append(words, 4);
                }
                body.Append(declarations.Data(), declarations.Size());
                if (threadId)
                {
                    u32 words[] = {kDclInput | (2u << 24), 0x00020072u};
                    body.Append(words, 2);
                }
                u32 temps[] = {kDclTemps | (2u << 24), Max(tempMax, 1u)};
                body.Append(temps, 2);
                bool compute = m.stage == Stage::Compute;
                if (compute)
                {
                    u32 words[] = {kDclThreadGroup | (4u << 24), m.groupSize[0], m.groupSize[1], m.groupSize[2]};
                    body.Append(words, 4);
                }
                body.Append(code.Data(), code.Size());

                // Structured buffers and compute exist from shader model 5.
                bool model5 = compute || !storages.Empty();
                Array<u8> shdr;
                AppendU32(shdr, (compute ? 0x00050000u : m.stage == Stage::Vertex ? 0x00010000u : 0u) | (model5 ? 0x50u : 0x40u));
                AppendU32(shdr, u32(body.Size()) + 2);
                Append(shdr, body.Data(), body.Size() * 4);

                Array<u8> isgn = Signature(inputs, false), osgn = Signature(outputs, true);
                struct Chunk
                {
                    const char* tag;
                    const Array<u8>* data;
                } chunks[] = {{"ISGN", &isgn}, {"OSGN", &osgn}, {model5 ? "SHEX" : "SHDR", &shdr}};
                Array<u8>& out = shader.bytes;
                out.Clear();
                Append(out, "DXBC", 4);
                for (int i = 0; i < 4; i++)
                    AppendU32(out, 0);
                AppendU32(out, 1);
                AppendU32(out, 0);
                AppendU32(out, 3);
                usize offsets = out.Size();
                for (int i = 0; i < 3; i++)
                    AppendU32(out, 0);
                for (int i = 0; i < 3; i++)
                {
                    u32 offset = u32(out.Size());
                    std::memcpy(out.Data() + offsets + usize(i) * 4, &offset, 4);
                    Append(out, chunks[i].tag, 4);
                    AppendU32(out, u32(chunks[i].data->Size()));
                    Append(out, chunks[i].data->Data(), chunks[i].data->Size());
                }
                u32 total = u32(out.Size());
                std::memcpy(out.Data() + 24, &total, 4);
                u32 sum[4];
                Checksum(out.Data() + 20, out.Size() - 20, sum);
                std::memcpy(out.Data() + 4, sum, 16);
                return true;
            }
        };
    }

    bool EmitDxbc(const Module& module, DxbcShader& out, String& error)
    {
        error.Clear();
        Generator g(module, error);
        return g.Run(out);
    }
}
