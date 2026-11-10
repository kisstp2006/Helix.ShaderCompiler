#include <Helix/ShaderCompiler/Assembly/DebugInfo.h>

#include <Helix/ShaderCompiler/Assembly/Implementation/Inflate.h>
#include <Helix/ShaderCompiler/Assembly/Signature.h>

#include <cstring>

namespace helix::shadercompiler
{
    namespace
    {
        constexpr u32 kDocument = 0x30, kMethodDebugInformation = 0x31;

        u32 Read(Span<const u8> data, usize at, u32 size)
        {
            if (at > data.Size() || size > data.Size() - at)
                return 0;
            u32 value = 0;
            for (u32 i = 0; i < size; i++)
                value |= u32(data[at + i]) << (8 * i);
            return value;
        }

        // Compressed signed integer (II.23.2): the sign is in the lowest bit of the rotated value.
        bool ReadSigned(Span<const u8> data, usize& at, i32& value)
        {
            usize start = at;
            u32 raw = 0;
            if (!ReadCompressed(data, at, raw))
                return false;
            usize size = at - start;
            u32 extend = size == 1 ? 0xFFFFFFC0u : size == 2 ? 0xFFFFE000u : 0xF0000000u;
            value = i32((raw >> 1) | ((raw & 1) ? extend : 0));
            return true;
        }
    }

    Span<const u8> DebugInfo::Blob(u32 index) const
    {
        usize at = index;
        u32 length = 0;
        if (!ReadCompressed(blobs_, at, length) || length > blobs_.Size() - at)
            return {};
        return Span<const u8>(blobs_.Data() + at, length);
    }

    u32 DebugInfo::Index(Span<const u8> table, usize at, u32 size) const { return Read(table, at, size); }

    bool DebugInfo::Load(Span<const u8> pdb)
    {
        loaded_ = false;
        if (Read(pdb, 0, 4) != 0x424A5342)
            return false;
        u32 versionLength = Read(pdb, 12, 4);
        if (versionLength > 255)
            return false;
        usize at = 16 + usize(versionLength);
        u32 streamCount = Read(pdb, at + 2, 2);
        at += 4;
        Span<const u8> tables, info;
        for (u32 i = 0; i < streamCount && at < pdb.Size(); i++)
        {
            u32 offset = Read(pdb, at, 4), size = Read(pdb, at + 4, 4);
            at += 8;
            char name[32] = {};
            u32 n = 0;
            while (at + n < pdb.Size() && pdb[at + n] != 0 && n < 31)
            {
                name[n] = char(pdb[at + n]);
                n++;
            }
            at += (usize(n) + 4) & ~usize(3);
            if (offset > pdb.Size() || size > pdb.Size() - offset)
                return false;
            Span<const u8> data(pdb.Data() + offset, size);
            if (!std::strcmp(name, "#~"))
                tables = data;
            else if (!std::strcmp(name, "#Pdb"))
                info = data;
            else if (!std::strcmp(name, "#Blob"))
                blobs_ = data;
        }
        if (tables.Size() < 24 || info.Size() < 32)
            return false;
        for (u32 i = 0; i < 16; i++)
            id_[i] = info[i];

        // Row counts: the debug tables from the table stream, the type system tables from the #Pdb stream.
        u32 rows[64] = {};
        u64 referenced = u64(Read(info, 24, 4)) | (u64(Read(info, 28, 4)) << 32);
        usize next = 32;
        for (u32 t = 0; t < 64; t++)
            if (referenced & (u64(1) << t))
            {
                rows[t] = Read(info, next, 4);
                next += 4;
            }
        u8 heaps = tables[6];
        blobIndex_ = (heaps & 4) ? 4 : 2;
        guidIndex_ = (heaps & 2) ? 4 : 2;
        u64 valid = u64(Read(tables, 8, 4)) | (u64(Read(tables, 12, 4)) << 32);
        next = 24;
        for (u32 t = 0; t < 64; t++)
            if (valid & (u64(1) << t))
            {
                rows[t] = Read(tables, next, 4);
                next += 4;
            }
        // Only the document and method tables are read; they are the first debug tables.
        if (valid & ((u64(1) << kDocument) - 1))
            return false;
        documentCount_ = rows[kDocument];
        methodCount_ = rows[kMethodDebugInformation];
        documentIndex_ = documentCount_ < 0x10000 ? 2 : 4;
        documentSize_ = blobIndex_ * 2 + guidIndex_ * 2;
        methodSize_ = documentIndex_ + blobIndex_;
        usize documentBytes = usize(documentCount_) * documentSize_, methodBytes = usize(methodCount_) * methodSize_;
        if (next > tables.Size() || documentBytes > tables.Size() - next || methodBytes > tables.Size() - next - documentBytes)
            return false;
        documents_ = Span<const u8>(tables.Data() + next, documentBytes);
        methods_ = Span<const u8>(tables.Data() + next + documentBytes, methodBytes);
        loaded_ = true;
        return true;
    }

    bool DebugInfo::LoadEmbedded(const Assembly& assembly)
    {
        // "MPDB", the size of the PDB, then the PDB as raw deflate data.
        Span<const u8> entry = assembly.DebugEntry(17);
        if (entry.Size() < 8 || Read(entry, 0, 4) != 0x4244504D)
            return false;
        u32 size = Read(entry, 4, 4);
        if (!Inflate(Span<const u8>(entry.Data() + 8, entry.Size() - 8), size, owned_) || owned_.Size() != size)
            return false;
        return Load(Span<const u8>(owned_.Data(), owned_.Size()));
    }

    bool DebugInfo::Matches(const Assembly& assembly) const
    {
        // CodeView entry: "RSDS", then the 16 byte id the PDB carries first.
        Span<const u8> entry = assembly.DebugEntry(2);
        if (!loaded_ || entry.Size() < 20 || Read(entry, 0, 4) != 0x53445352)
            return false;
        for (u32 i = 0; i < 16; i++)
            if (entry[4 + i] != id_[i])
                return false;
        return true;
    }

    bool DebugInfo::Find(u32 methodRow, u32 ilOffset, String& file, u32& line) const
    {
        if (!loaded_ || methodRow == 0 || methodRow > methodCount_)
            return false;
        usize row = usize(methodRow - 1) * methodSize_;
        u32 document = Index(methods_, row, documentIndex_);
        Span<const u8> points = Blob(Index(methods_, row + documentIndex_, blobIndex_));
        if (points.Empty())
            return false;
        usize at = 0;
        u32 value = 0;
        if (!ReadCompressed(points, at, value))
            return false;
        if (document == 0 && !ReadCompressed(points, at, document))
            return false;
        u32 offset = 0, startLine = 0, bestLine = 0, bestDocument = 0, firstLine = 0, firstDocument = 0;
        bool first = true, seen = false, found = false;
        while (at < points.Size())
        {
            u32 delta = 0;
            if (!ReadCompressed(points, at, delta))
                return false;
            if (delta == 0 && !first)
            {
                // A document record.
                if (!ReadCompressed(points, at, document))
                    return false;
                continue;
            }
            offset += delta;
            first = false;
            u32 lines = 0;
            if (!ReadCompressed(points, at, lines))
                return false;
            if (lines == 0)
            {
                u32 columns = 0;
                if (!ReadCompressed(points, at, columns))
                    return false;
                if (columns == 0)
                    continue;  // hidden
            }
            else
            {
                i32 columns = 0;
                if (!ReadSigned(points, at, columns))
                    return false;
            }
            if (!seen)
            {
                u32 column = 0;
                if (!ReadCompressed(points, at, startLine) || !ReadCompressed(points, at, column))
                    return false;
            }
            else
            {
                i32 dLine = 0, dColumn = 0;
                if (!ReadSigned(points, at, dLine) || !ReadSigned(points, at, dColumn))
                    return false;
                startLine = u32(i32(startLine) + dLine);
            }
            if (!seen)
            {
                firstLine = startLine;
                firstDocument = document;
            }
            seen = true;
            if (offset <= ilOffset)
            {
                bestLine = startLine;
                bestDocument = document;
                found = true;
            }
        }
        if (!seen)
            return false;
        if (!found)
        {
            bestLine = firstLine;
            bestDocument = firstDocument;
        }
        if (bestDocument == 0 || bestDocument > documentCount_)
            return false;
        // The name blob: a separator, then the blob indices of the parts.
        Span<const u8> name = Blob(Index(documents_, usize(bestDocument - 1) * documentSize_, blobIndex_));
        if (name.Empty())
            return false;
        file.Clear();
        char separator = char(name[0]);
        usize p = 1;
        bool firstPart = true;
        while (p < name.Size())
        {
            u32 part = 0;
            if (!ReadCompressed(name, p, part))
                return false;
            if (!firstPart && separator)
                file.Append(separator);
            firstPart = false;
            Span<const u8> text = part ? Blob(part) : Span<const u8>();
            file.Append(StringView(reinterpret_cast<const char*>(text.Data()), text.Size()));
        }
        line = bestLine;
        return true;
    }
}
