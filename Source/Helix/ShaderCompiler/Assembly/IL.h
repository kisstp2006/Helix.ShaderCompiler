#pragma once

#include <Helix/Core/Basics/Span.h>
#include <Helix/Core/Basics/Types.h>

namespace helix::shadercompiler
{
    enum class OperandKind : u8
    {
        None,
        Int8,      // signed constant
        Int32,
        Int64,
        Float32,
        Float64,
        Variable8, // argument or local index
        Variable16,
        Token,     // metadata token
        Branch8,   // target as an absolute IL offset in `target`
        Branch32,
        Switch,    // `switchCount` targets, read with SwitchTarget
    };

    // X(name, code, mnemonic, operand); two byte opcodes are 0xFE00 | second byte.
#define HX_IL_OPCODES(X)                                                                                                  \
    X(Nop, 0x00, "nop", None) X(Break, 0x01, "break", None) X(Ldarg0, 0x02, "ldarg.0", None) X(Ldarg1, 0x03, "ldarg.1", None)  \
    X(Ldarg2, 0x04, "ldarg.2", None) X(Ldarg3, 0x05, "ldarg.3", None) X(Ldloc0, 0x06, "ldloc.0", None)                    \
    X(Ldloc1, 0x07, "ldloc.1", None) X(Ldloc2, 0x08, "ldloc.2", None) X(Ldloc3, 0x09, "ldloc.3", None)                    \
    X(Stloc0, 0x0A, "stloc.0", None) X(Stloc1, 0x0B, "stloc.1", None) X(Stloc2, 0x0C, "stloc.2", None)                    \
    X(Stloc3, 0x0D, "stloc.3", None) X(LdargS, 0x0E, "ldarg.s", Variable8) X(LdargaS, 0x0F, "ldarga.s", Variable8)        \
    X(StargS, 0x10, "starg.s", Variable8) X(LdlocS, 0x11, "ldloc.s", Variable8) X(LdlocaS, 0x12, "ldloca.s", Variable8)   \
    X(StlocS, 0x13, "stloc.s", Variable8) X(Ldnull, 0x14, "ldnull", None) X(LdcI4M1, 0x15, "ldc.i4.m1", None)             \
    X(LdcI4_0, 0x16, "ldc.i4.0", None) X(LdcI4_1, 0x17, "ldc.i4.1", None) X(LdcI4_2, 0x18, "ldc.i4.2", None)              \
    X(LdcI4_3, 0x19, "ldc.i4.3", None) X(LdcI4_4, 0x1A, "ldc.i4.4", None) X(LdcI4_5, 0x1B, "ldc.i4.5", None)              \
    X(LdcI4_6, 0x1C, "ldc.i4.6", None) X(LdcI4_7, 0x1D, "ldc.i4.7", None) X(LdcI4_8, 0x1E, "ldc.i4.8", None)              \
    X(LdcI4S, 0x1F, "ldc.i4.s", Int8) X(LdcI4, 0x20, "ldc.i4", Int32) X(LdcI8, 0x21, "ldc.i8", Int64)                     \
    X(LdcR4, 0x22, "ldc.r4", Float32) X(LdcR8, 0x23, "ldc.r8", Float64) X(Dup, 0x25, "dup", None) X(Pop, 0x26, "pop", None) \
    X(Jmp, 0x27, "jmp", Token) X(Call, 0x28, "call", Token) X(Calli, 0x29, "calli", Token) X(Ret, 0x2A, "ret", None)      \
    X(BrS, 0x2B, "br.s", Branch8) X(BrfalseS, 0x2C, "brfalse.s", Branch8) X(BrtrueS, 0x2D, "brtrue.s", Branch8)           \
    X(BeqS, 0x2E, "beq.s", Branch8) X(BgeS, 0x2F, "bge.s", Branch8) X(BgtS, 0x30, "bgt.s", Branch8)                       \
    X(BleS, 0x31, "ble.s", Branch8) X(BltS, 0x32, "blt.s", Branch8) X(BneUnS, 0x33, "bne.un.s", Branch8)                  \
    X(BgeUnS, 0x34, "bge.un.s", Branch8) X(BgtUnS, 0x35, "bgt.un.s", Branch8) X(BleUnS, 0x36, "ble.un.s", Branch8)        \
    X(BltUnS, 0x37, "blt.un.s", Branch8) X(Br, 0x38, "br", Branch32) X(Brfalse, 0x39, "brfalse", Branch32)                \
    X(Brtrue, 0x3A, "brtrue", Branch32) X(Beq, 0x3B, "beq", Branch32) X(Bge, 0x3C, "bge", Branch32)                       \
    X(Bgt, 0x3D, "bgt", Branch32) X(Ble, 0x3E, "ble", Branch32) X(Blt, 0x3F, "blt", Branch32)                             \
    X(BneUn, 0x40, "bne.un", Branch32) X(BgeUn, 0x41, "bge.un", Branch32) X(BgtUn, 0x42, "bgt.un", Branch32)              \
    X(BleUn, 0x43, "ble.un", Branch32) X(BltUn, 0x44, "blt.un", Branch32) X(Switch, 0x45, "switch", Switch)               \
    X(LdindI1, 0x46, "ldind.i1", None) X(LdindU1, 0x47, "ldind.u1", None) X(LdindI2, 0x48, "ldind.i2", None)              \
    X(LdindU2, 0x49, "ldind.u2", None) X(LdindI4, 0x4A, "ldind.i4", None) X(LdindU4, 0x4B, "ldind.u4", None)              \
    X(LdindI8, 0x4C, "ldind.i8", None) X(LdindI, 0x4D, "ldind.i", None) X(LdindR4, 0x4E, "ldind.r4", None)                \
    X(LdindR8, 0x4F, "ldind.r8", None) X(LdindRef, 0x50, "ldind.ref", None) X(StindRef, 0x51, "stind.ref", None)          \
    X(StindI1, 0x52, "stind.i1", None) X(StindI2, 0x53, "stind.i2", None) X(StindI4, 0x54, "stind.i4", None)              \
    X(StindI8, 0x55, "stind.i8", None) X(StindR4, 0x56, "stind.r4", None) X(StindR8, 0x57, "stind.r8", None)              \
    X(Add, 0x58, "add", None) X(Sub, 0x59, "sub", None) X(Mul, 0x5A, "mul", None) X(Div, 0x5B, "div", None)               \
    X(DivUn, 0x5C, "div.un", None) X(Rem, 0x5D, "rem", None) X(RemUn, 0x5E, "rem.un", None) X(And, 0x5F, "and", None)     \
    X(Or, 0x60, "or", None) X(Xor, 0x61, "xor", None) X(Shl, 0x62, "shl", None) X(Shr, 0x63, "shr", None)                 \
    X(ShrUn, 0x64, "shr.un", None) X(Neg, 0x65, "neg", None) X(Not, 0x66, "not", None) X(ConvI1, 0x67, "conv.i1", None)   \
    X(ConvI2, 0x68, "conv.i2", None) X(ConvI4, 0x69, "conv.i4", None) X(ConvI8, 0x6A, "conv.i8", None)                    \
    X(ConvR4, 0x6B, "conv.r4", None) X(ConvR8, 0x6C, "conv.r8", None) X(ConvU4, 0x6D, "conv.u4", None)                    \
    X(ConvU8, 0x6E, "conv.u8", None) X(Callvirt, 0x6F, "callvirt", Token) X(Cpobj, 0x70, "cpobj", Token)                  \
    X(Ldobj, 0x71, "ldobj", Token) X(Ldstr, 0x72, "ldstr", Token) X(Newobj, 0x73, "newobj", Token)                        \
    X(Castclass, 0x74, "castclass", Token) X(Isinst, 0x75, "isinst", Token) X(ConvRUn, 0x76, "conv.r.un", None)           \
    X(Unbox, 0x79, "unbox", Token) X(Throw, 0x7A, "throw", None) X(Ldfld, 0x7B, "ldfld", Token)                           \
    X(Ldflda, 0x7C, "ldflda", Token) X(Stfld, 0x7D, "stfld", Token) X(Ldsfld, 0x7E, "ldsfld", Token)                      \
    X(Ldsflda, 0x7F, "ldsflda", Token) X(Stsfld, 0x80, "stsfld", Token) X(Stobj, 0x81, "stobj", Token)                    \
    X(ConvOvfI1Un, 0x82, "conv.ovf.i1.un", None) X(ConvOvfI2Un, 0x83, "conv.ovf.i2.un", None)                             \
    X(ConvOvfI4Un, 0x84, "conv.ovf.i4.un", None) X(ConvOvfI8Un, 0x85, "conv.ovf.i8.un", None)                             \
    X(ConvOvfU1Un, 0x86, "conv.ovf.u1.un", None) X(ConvOvfU2Un, 0x87, "conv.ovf.u2.un", None)                             \
    X(ConvOvfU4Un, 0x88, "conv.ovf.u4.un", None) X(ConvOvfU8Un, 0x89, "conv.ovf.u8.un", None)                             \
    X(ConvOvfIUn, 0x8A, "conv.ovf.i.un", None) X(ConvOvfUUn, 0x8B, "conv.ovf.u.un", None) X(Box, 0x8C, "box", Token)      \
    X(Newarr, 0x8D, "newarr", Token) X(Ldlen, 0x8E, "ldlen", None) X(Ldelema, 0x8F, "ldelema", Token)                     \
    X(LdelemI1, 0x90, "ldelem.i1", None) X(LdelemU1, 0x91, "ldelem.u1", None) X(LdelemI2, 0x92, "ldelem.i2", None)        \
    X(LdelemU2, 0x93, "ldelem.u2", None) X(LdelemI4, 0x94, "ldelem.i4", None) X(LdelemU4, 0x95, "ldelem.u4", None)        \
    X(LdelemI8, 0x96, "ldelem.i8", None) X(LdelemI, 0x97, "ldelem.i", None) X(LdelemR4, 0x98, "ldelem.r4", None)          \
    X(LdelemR8, 0x99, "ldelem.r8", None) X(LdelemRef, 0x9A, "ldelem.ref", None) X(StelemI, 0x9B, "stelem.i", None)        \
    X(StelemI1, 0x9C, "stelem.i1", None) X(StelemI2, 0x9D, "stelem.i2", None) X(StelemI4, 0x9E, "stelem.i4", None)        \
    X(StelemI8, 0x9F, "stelem.i8", None) X(StelemR4, 0xA0, "stelem.r4", None) X(StelemR8, 0xA1, "stelem.r8", None)        \
    X(StelemRef, 0xA2, "stelem.ref", None) X(Ldelem, 0xA3, "ldelem", Token) X(Stelem, 0xA4, "stelem", Token)              \
    X(UnboxAny, 0xA5, "unbox.any", Token) X(ConvOvfI1, 0xB3, "conv.ovf.i1", None) X(ConvOvfU1, 0xB4, "conv.ovf.u1", None) \
    X(ConvOvfI2, 0xB5, "conv.ovf.i2", None) X(ConvOvfU2, 0xB6, "conv.ovf.u2", None) X(ConvOvfI4, 0xB7, "conv.ovf.i4", None) \
    X(ConvOvfU4, 0xB8, "conv.ovf.u4", None) X(ConvOvfI8, 0xB9, "conv.ovf.i8", None) X(ConvOvfU8, 0xBA, "conv.ovf.u8", None) \
    X(Refanyval, 0xC2, "refanyval", Token) X(Ckfinite, 0xC3, "ckfinite", None) X(Mkrefany, 0xC6, "mkrefany", Token)       \
    X(Ldtoken, 0xD0, "ldtoken", Token) X(ConvU2, 0xD1, "conv.u2", None) X(ConvU1, 0xD2, "conv.u1", None)                  \
    X(ConvI, 0xD3, "conv.i", None) X(ConvOvfI, 0xD4, "conv.ovf.i", None) X(ConvOvfU, 0xD5, "conv.ovf.u", None)            \
    X(AddOvf, 0xD6, "add.ovf", None) X(AddOvfUn, 0xD7, "add.ovf.un", None) X(MulOvf, 0xD8, "mul.ovf", None)               \
    X(MulOvfUn, 0xD9, "mul.ovf.un", None) X(SubOvf, 0xDA, "sub.ovf", None) X(SubOvfUn, 0xDB, "sub.ovf.un", None)          \
    X(Endfinally, 0xDC, "endfinally", None) X(Leave, 0xDD, "leave", Branch32) X(LeaveS, 0xDE, "leave.s", Branch8)         \
    X(StindI, 0xDF, "stind.i", None) X(ConvU, 0xE0, "conv.u", None) X(Arglist, 0xFE00, "arglist", None)                   \
    X(Ceq, 0xFE01, "ceq", None) X(Cgt, 0xFE02, "cgt", None) X(CgtUn, 0xFE03, "cgt.un", None) X(Clt, 0xFE04, "clt", None)  \
    X(CltUn, 0xFE05, "clt.un", None) X(Ldftn, 0xFE06, "ldftn", Token) X(Ldvirtftn, 0xFE07, "ldvirtftn", Token)            \
    X(Ldarg, 0xFE09, "ldarg", Variable16) X(Ldarga, 0xFE0A, "ldarga", Variable16) X(Starg, 0xFE0B, "starg", Variable16)   \
    X(Ldloc, 0xFE0C, "ldloc", Variable16) X(Ldloca, 0xFE0D, "ldloca", Variable16) X(Stloc, 0xFE0E, "stloc", Variable16)   \
    X(Localloc, 0xFE0F, "localloc", None) X(Endfilter, 0xFE11, "endfilter", None) X(Unaligned, 0xFE12, "unaligned.", Int8) \
    X(Volatile, 0xFE13, "volatile.", None) X(Tail, 0xFE14, "tail.", None) X(Initobj, 0xFE15, "initobj", Token)            \
    X(Constrained, 0xFE16, "constrained.", Token) X(Cpblk, 0xFE17, "cpblk", None) X(Initblk, 0xFE18, "initblk", None)     \
    X(No, 0xFE19, "no.", Int8) X(Rethrow, 0xFE1A, "rethrow", None) X(Sizeof, 0xFE1C, "sizeof", Token)                     \
    X(Refanytype, 0xFE1D, "refanytype", None) X(Readonly, 0xFE1E, "readonly.", None)

    enum class Opcode : u16
    {
#define HX_IL_ENUM(name, code, mnemonic, operand) name = code,
        HX_IL_OPCODES(HX_IL_ENUM)
#undef HX_IL_ENUM
    };

    struct Instruction
    {
        u32 offset = 0;
        u32 size = 0;
        Opcode opcode = Opcode::Nop;
        OperandKind operand = OperandKind::None;
        // Constant, variable index or metadata token.
        i64 value = 0;
        f64 number = 0;
        // Absolute IL offset of a branch target.
        u32 target = 0;
        u32 switchCount = 0;
        const u8* switchData = nullptr;

        // Absolute IL offset of the n-th switch target.
        u32 SwitchTarget(u32 n) const;
    };

    // Decodes the instruction at `offset` and advances it; false at an unknown opcode or when the code ends early.
    bool DecodeInstruction(Span<const u8> code, u32& offset, Instruction& out);
    const char* OpcodeName(Opcode opcode);
}
