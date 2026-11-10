#include <Helix/ShaderCompiler/Assembly/IL.h>

#include <cstring>

namespace helix::shadercompiler
{
    namespace
    {
        struct OpcodeInfo
        {
            const char* name;
            OperandKind operand;
        };

        // Indexed by the one byte opcode, and by 256 + second byte for the 0xFE prefixed ones.
        struct OpcodeTable
        {
            OpcodeInfo entries[256 + 32] = {};
            constexpr OpcodeTable()
            {
#define HX_IL_ENTRY(id, code, mnemonic, kind) entries[(code) >= 0xFE00 ? 256 + ((code) & 0xFF) : (code)] = {mnemonic, OperandKind::kind};
                HX_IL_OPCODES(HX_IL_ENTRY)
#undef HX_IL_ENTRY
            }
        };

        constexpr OpcodeTable kTable;

        const OpcodeInfo* Lookup(u32 code)
        {
            u32 index = code >= 0xFE00 ? 256 + (code & 0xFF) : code;
            if (index >= 256 + 32 || (code >= 0xFE00 && (code & 0xFF) >= 32))
                return nullptr;
            return kTable.entries[index].name ? &kTable.entries[index] : nullptr;
        }

        template <typename T>
        T Read(const u8* p)
        {
            T v;
            std::memcpy(&v, p, sizeof(T));
            return v;
        }
    }

    u32 Instruction::SwitchTarget(u32 n) const { return n < switchCount ? u32(i32(offset + size) + Read<i32>(switchData + usize(n) * 4)) : 0; }

    const char* OpcodeName(Opcode opcode)
    {
        const OpcodeInfo* info = Lookup(u32(opcode));
        return info ? info->name : "?";
    }

    bool DecodeInstruction(Span<const u8> code, u32& offset, Instruction& out)
    {
        out = Instruction();
        usize at = offset, size = code.Size();
        if (at >= size)
            return false;
        u32 op = code[at++];
        if (op == 0xFE)
        {
            if (at >= size)
                return false;
            op = 0xFE00 | code[at++];
        }
        const OpcodeInfo* info = Lookup(op);
        if (!info)
            return false;
        out.offset = offset;
        out.opcode = Opcode(op);
        out.operand = info->operand;
        usize operandSize = 0;
        switch (info->operand)
        {
        case OperandKind::None: break;
        case OperandKind::Int8: case OperandKind::Variable8: case OperandKind::Branch8: operandSize = 1; break;
        case OperandKind::Variable16: operandSize = 2; break;
        case OperandKind::Int32: case OperandKind::Float32: case OperandKind::Token: case OperandKind::Branch32: case OperandKind::Switch: operandSize = 4; break;
        case OperandKind::Int64: case OperandKind::Float64: operandSize = 8; break;
        }
        if (size - at < operandSize)
            return false;
        const u8* p = code.Data() + at;
        at += operandSize;
        switch (info->operand)
        {
        case OperandKind::None: break;
        case OperandKind::Int8: out.value = Read<i8>(p); break;
        case OperandKind::Variable8: out.value = Read<u8>(p); break;
        case OperandKind::Variable16: out.value = Read<u16>(p); break;
        case OperandKind::Int32: out.value = Read<i32>(p); break;
        case OperandKind::Token: out.value = Read<u32>(p); break;
        case OperandKind::Int64: out.value = Read<i64>(p); break;
        case OperandKind::Float32: out.number = Read<f32>(p); break;
        case OperandKind::Float64: out.number = Read<f64>(p); break;
        case OperandKind::Branch8: out.target = u32(i32(at) + Read<i8>(p)); break;
        case OperandKind::Branch32: out.target = u32(i32(at) + Read<i32>(p)); break;
        case OperandKind::Switch:
        {
            u32 count = Read<u32>(p);
            if ((size - at) / 4 < count)
                return false;
            out.switchCount = count;
            out.switchData = code.Data() + at;
            at += usize(count) * 4;
            break;
        }
        }
        out.size = u32(at - offset);
        offset = u32(at);
        return true;
    }
}
