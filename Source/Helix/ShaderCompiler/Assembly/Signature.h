#pragma once

#include <Helix/ShaderCompiler/Assembly/Assembly.h>

namespace helix::shadercompiler
{
    // Element types of signatures (ECMA-335 II.23.1.16).
    enum class ElementType : u8
    {
        End = 0x00, Void = 0x01, Boolean = 0x02, Char = 0x03, I1 = 0x04, U1 = 0x05, I2 = 0x06, U2 = 0x07, I4 = 0x08, U4 = 0x09,
        I8 = 0x0A, U8 = 0x0B, R4 = 0x0C, R8 = 0x0D, String = 0x0E, Ptr = 0x0F, ByRef = 0x10, ValueType = 0x11, Class = 0x12,
        Var = 0x13, Array = 0x14, GenericInst = 0x15, TypedByRef = 0x16, I = 0x18, U = 0x19, FnPtr = 0x1B, Object = 0x1C,
        SzArray = 0x1D, MVar = 0x1E,
    };

    // A type in a signature. Ptr, ByRef, SzArray and Array hold their element in args[0]; GenericInst holds the generic
    // type in `token` and the type arguments in args; Var and MVar hold the generic parameter number in `index`.
    struct TypeSig
    {
        ElementType kind = ElementType::End;
        Token token;
        u32 index = 0;
        bool genericValueType = false;
        bool pinned = false;
        Array<TypeSig> args;
    };

    struct MethodSig
    {
        bool hasThis = false;
        u32 genericParamCount = 0;
        TypeSig returnType;
        Array<TypeSig> params;
    };

    bool ParseMethodSignature(Span<const u8> blob, MethodSig& out);
    bool ParseFieldSignature(Span<const u8> blob, TypeSig& out);
    bool ParseLocalsSignature(Span<const u8> blob, Array<TypeSig>& out);
    bool ParseTypeSpec(Span<const u8> blob, TypeSig& out);
    // Type arguments of a generic method instantiation (MethodSpec).
    bool ParseMethodSpec(Span<const u8> blob, Array<TypeSig>& out);

    struct AttributeArgument
    {
        enum class Kind : u8
        {
            Integer,
            Number,
            Boolean,
            Text,
        };
        Kind kind = Kind::Integer;
        // Set for named arguments (properties and fields), empty for constructor arguments.
        StringView name;
        i64 integer = 0;
        f64 number = 0;
        StringView text;
    };

    // Decodes the constructor arguments and the named arguments of a custom attribute. Enum arguments are read as
    // 32 bit integers; arrays and boxed objects are not supported.
    bool DecodeAttribute(const Assembly& assembly, const CustomAttribute& attribute, Array<AttributeArgument>& fixed, Array<AttributeArgument>& named);

    // Compressed unsigned integer of blobs and signatures (II.23.2); false when the data ends early.
    inline bool ReadCompressed(Span<const u8> data, usize& at, u32& value)
    {
        if (at >= data.Size())
            return false;
        u8 first = data[at];
        if ((first & 0x80) == 0)
        {
            value = first;
            at += 1;
            return true;
        }
        if ((first & 0xC0) == 0x80)
        {
            if (at + 2 > data.Size())
                return false;
            value = (u32(first & 0x3F) << 8) | data[at + 1];
            at += 2;
            return true;
        }
        if (at + 4 > data.Size() || (first & 0xE0) != 0xC0)
            return false;
        value = (u32(first & 0x1F) << 24) | (u32(data[at + 1]) << 16) | (u32(data[at + 2]) << 8) | data[at + 3];
        at += 4;
        return true;
    }
}
