#include <Helix/ShaderCompiler/Assembly/Implementation/Inflate.h>

namespace helix::shadercompiler
{
    namespace
    {
        struct Bits
        {
            Span<const u8> in;
            usize at = 0;
            u32 buffer = 0, count = 0;
            bool failed = false;

            u32 Take(u32 n)
            {
                while (count < n)
                {
                    if (at >= in.Size())
                    {
                        failed = true;
                        return 0;
                    }
                    buffer |= u32(in[at++]) << count;
                    count += 8;
                }
                u32 value = buffer & ((1u << n) - 1);
                buffer >>= n;
                count -= n;
                return value;
            }
        };

        // Canonical Huffman code: the number of codes per length and the symbols in code order.
        struct Code
        {
            u16 counts[16] = {};
            u16 symbols[288] = {};
        };

        bool Build(Code& code, const u8* lengths, u32 n)
        {
            for (u16& c : code.counts)
                c = 0;
            for (u32 i = 0; i < n; i++)
                code.counts[lengths[i]]++;
            code.counts[0] = 0;
            i32 left = 1;
            for (u32 length = 1; length < 16; length++)
            {
                left = left * 2 - code.counts[length];
                if (left < 0)
                    return false;
            }
            u16 offsets[16] = {};
            for (u32 length = 1; length < 15; length++)
                offsets[length + 1] = u16(offsets[length] + code.counts[length]);
            for (u32 i = 0; i < n; i++)
                if (lengths[i])
                    code.symbols[offsets[lengths[i]]++] = u16(i);
            return true;
        }

        i32 Decode(Bits& bits, const Code& code)
        {
            i32 value = 0, first = 0, index = 0;
            for (u32 length = 1; length < 16; length++)
            {
                value |= i32(bits.Take(1));
                i32 count = code.counts[length];
                if (value - count < first)
                    return code.symbols[index + (value - first)];
                index += count;
                first = (first + count) << 1;
                value <<= 1;
            }
            return -1;
        }

        constexpr u16 kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr u8 kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        constexpr u16 kDistanceBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                           193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        constexpr u8 kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    }

    bool Inflate(Span<const u8> in, usize limit, Array<u8>& out)
    {
        out.Clear();
        Bits bits{in};
        Code literals, distances;
        bool last = false;
        while (!last)
        {
            last = bits.Take(1) != 0;
            u32 type = bits.Take(2);
            if (bits.failed || type == 3)
                return false;
            if (type == 0)
            {
                // Stored block: byte aligned length and its complement.
                bits.buffer = 0;
                bits.count = 0;
                if (bits.at + 4 > in.Size())
                    return false;
                u32 length = u32(in[bits.at]) | (u32(in[bits.at + 1]) << 8);
                u32 check = u32(in[bits.at + 2]) | (u32(in[bits.at + 3]) << 8);
                bits.at += 4;
                if ((length ^ 0xFFFF) != check || bits.at + length > in.Size() || out.Size() + length > limit)
                    return false;
                for (u32 i = 0; i < length; i++)
                    out.Push(in[bits.at + i]);
                bits.at += length;
                continue;
            }
            u8 lengths[320] = {};
            if (type == 1)
            {
                for (u32 i = 0; i < 288; i++)
                    lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                for (u32 i = 0; i < 30; i++)
                    lengths[288 + i] = 5;
                if (!Build(literals, lengths, 288) || !Build(distances, lengths + 288, 30))
                    return false;
            }
            else
            {
                u32 literalCount = bits.Take(5) + 257, distanceCount = bits.Take(5) + 1, lengthCount = bits.Take(4) + 4;
                if (literalCount > 286 || distanceCount > 30)
                    return false;
                static constexpr u8 kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                u8 codeLengths[19] = {};
                for (u32 i = 0; i < lengthCount; i++)
                    codeLengths[kOrder[i]] = u8(bits.Take(3));
                Code lengthCode;
                if (!Build(lengthCode, codeLengths, 19))
                    return false;
                for (u32 i = 0; i < literalCount + distanceCount;)
                {
                    i32 symbol = Decode(bits, lengthCode);
                    if (symbol < 0 || bits.failed)
                        return false;
                    if (symbol < 16)
                    {
                        lengths[i++] = u8(symbol);
                        continue;
                    }
                    u8 repeated = 0;
                    u32 times = 0;
                    if (symbol == 16)
                    {
                        if (i == 0)
                            return false;
                        repeated = lengths[i - 1];
                        times = 3 + bits.Take(2);
                    }
                    else
                        times = symbol == 17 ? 3 + bits.Take(3) : 11 + bits.Take(7);
                    if (i + times > literalCount + distanceCount)
                        return false;
                    while (times--)
                        lengths[i++] = repeated;
                }
                if (lengths[256] == 0 || !Build(literals, lengths, literalCount) || !Build(distances, lengths + literalCount, distanceCount))
                    return false;
            }
            for (;;)
            {
                i32 symbol = Decode(bits, literals);
                if (symbol < 0 || bits.failed)
                    return false;
                if (symbol < 256)
                {
                    if (out.Size() >= limit)
                        return false;
                    out.Push(u8(symbol));
                    continue;
                }
                if (symbol == 256)
                    break;
                symbol -= 257;
                if (symbol >= 29)
                    return false;
                u32 length = kLengthBase[symbol] + bits.Take(kLengthExtra[symbol]);
                i32 d = Decode(bits, distances);
                if (d < 0 || d >= 30 || bits.failed)
                    return false;
                usize distance = kDistanceBase[d] + bits.Take(kDistanceExtra[d]);
                if (distance > out.Size() || out.Size() + length > limit)
                    return false;
                for (u32 i = 0; i < length; i++)
                {
                    // A copy first: Push may move the array.
                    u8 byte = out[out.Size() - distance];
                    out.Push(byte);
                }
            }
        }
        return !bits.failed;
    }
}
