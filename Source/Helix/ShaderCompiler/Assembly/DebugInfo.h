#pragma once

#include <Helix/ShaderCompiler/Assembly/Assembly.h>

namespace helix::shadercompiler
{
    // Source positions of a portable PDB: the file and line of an IL offset of a method. Reads a PDB file next to the
    // assembly or the one embedded in it. Damaged data fails to load or finds nothing; it is never read out of bounds.
    class DebugInfo
    {
    public:
        // A portable PDB file; the bytes must stay alive while the object is used.
        bool Load(Span<const u8> pdb);
        // The PDB embedded in the assembly (debug directory entry 17), decompressed into memory of its own.
        bool LoadEmbedded(const Assembly& assembly);

        // True when the PDB was written for this assembly (the id of its CodeView debug entry).
        bool Matches(const Assembly& assembly) const;

        // The sequence point of the method that covers the IL offset (the last one at or before it, else the first).
        bool Find(u32 methodRow, u32 ilOffset, String& file, u32& line) const;

    private:
        Span<const u8> Blob(u32 index) const;
        u32 Index(Span<const u8> table, usize at, u32 size) const;

        Array<u8> owned_;
        u8 id_[16] = {};
        Span<const u8> blobs_;
        Span<const u8> documents_, methods_;
        u32 documentCount_ = 0, documentSize_ = 0, methodCount_ = 0, methodSize_ = 0;
        u32 blobIndex_ = 2, guidIndex_ = 2, documentIndex_ = 2;
        bool loaded_ = false;
    };
}
