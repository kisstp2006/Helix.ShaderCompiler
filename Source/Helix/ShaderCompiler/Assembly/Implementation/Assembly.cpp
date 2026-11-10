#include <Helix/ShaderCompiler/Assembly/Assembly.h>
#include <Helix/ShaderCompiler/Assembly/Signature.h>

#include <cstring>

namespace helix::shadercompiler
{
    namespace
    {
        // Column kinds of the table schemas.
        constexpr u8 kU16 = 1, kU32 = 2, kStr = 3, kGuid = 4, kBlob = 5, kTable = 0x40, kCoded = 0x80;
        constexpr u8 T(Table t) { return u8(kTable | u8(t)); }

        enum Coded : u8
        {
            TypeDefOrRef, HasConstant, HasCustomAttribute, HasFieldMarshal, HasDeclSecurity, MemberRefParent, HasSemantics,
            MethodDefOrRef, MemberForwarded, Implementation, CustomAttributeType, ResolutionScope, TypeOrMethodDef, CodedCount,
        };
        constexpr u8 C(Coded c) { return u8(kCoded | c); }

        constexpr u8 kNone = 0xFF;
        struct CodedIndex
        {
            u8 bits;
            u8 count;
            u8 tables[22];
        };

        constexpr u8 X(Table t) { return u8(t); }
        constexpr CodedIndex kCodedIndexes[CodedCount] = {
            {2, 3, {X(Table::TypeDef), X(Table::TypeRef), X(Table::TypeSpec)}},
            {2, 3, {X(Table::Field), X(Table::Param), X(Table::Property)}},
            {5, 22, {X(Table::MethodDef), X(Table::Field), X(Table::TypeRef), X(Table::TypeDef), X(Table::Param), X(Table::InterfaceImpl),
                     X(Table::MemberRef), X(Table::Module), X(Table::DeclSecurity), X(Table::Property), X(Table::Event), X(Table::StandAloneSig),
                     X(Table::ModuleRef), X(Table::TypeSpec), X(Table::Assembly), X(Table::AssemblyRef), X(Table::File), X(Table::ExportedType),
                     X(Table::ManifestResource), X(Table::GenericParam), X(Table::GenericParamConstraint), X(Table::MethodSpec)}},
            {1, 2, {X(Table::Field), X(Table::Param)}},
            {2, 3, {X(Table::TypeDef), X(Table::MethodDef), X(Table::Assembly)}},
            {3, 5, {X(Table::TypeDef), X(Table::TypeRef), X(Table::ModuleRef), X(Table::MethodDef), X(Table::TypeSpec)}},
            {1, 2, {X(Table::Event), X(Table::Property)}},
            {1, 2, {X(Table::MethodDef), X(Table::MemberRef)}},
            {1, 2, {X(Table::Field), X(Table::MethodDef)}},
            {2, 3, {X(Table::File), X(Table::AssemblyRef), X(Table::ExportedType)}},
            {3, 5, {kNone, kNone, X(Table::MethodDef), X(Table::MemberRef), kNone}},
            {2, 4, {X(Table::Module), X(Table::ModuleRef), X(Table::AssemblyRef), X(Table::TypeRef)}},
            {1, 2, {X(Table::TypeDef), X(Table::MethodDef)}},
        };

        // Columns of every table in file order, zero terminated.
        constexpr u8 kSchemas[usize(Table::Count)][10] = {
            /* Module */ {kU16, kStr, kGuid, kGuid, kGuid},
            /* TypeRef */ {C(ResolutionScope), kStr, kStr},
            /* TypeDef */ {kU32, kStr, kStr, C(TypeDefOrRef), T(Table::Field), T(Table::MethodDef)},
            /* FieldPtr */ {T(Table::Field)},
            /* Field */ {kU16, kStr, kBlob},
            /* MethodPtr */ {T(Table::MethodDef)},
            /* MethodDef */ {kU32, kU16, kU16, kStr, kBlob, T(Table::Param)},
            /* ParamPtr */ {T(Table::Param)},
            /* Param */ {kU16, kU16, kStr},
            /* InterfaceImpl */ {T(Table::TypeDef), C(TypeDefOrRef)},
            /* MemberRef */ {C(MemberRefParent), kStr, kBlob},
            /* Constant */ {kU16, C(HasConstant), kBlob},
            /* CustomAttribute */ {C(HasCustomAttribute), C(CustomAttributeType), kBlob},
            /* FieldMarshal */ {C(HasFieldMarshal), kBlob},
            /* DeclSecurity */ {kU16, C(HasDeclSecurity), kBlob},
            /* ClassLayout */ {kU16, kU32, T(Table::TypeDef)},
            /* FieldLayout */ {kU32, T(Table::Field)},
            /* StandAloneSig */ {kBlob},
            /* EventMap */ {T(Table::TypeDef), T(Table::Event)},
            /* EventPtr */ {T(Table::Event)},
            /* Event */ {kU16, kStr, C(TypeDefOrRef)},
            /* PropertyMap */ {T(Table::TypeDef), T(Table::Property)},
            /* PropertyPtr */ {T(Table::Property)},
            /* Property */ {kU16, kStr, kBlob},
            /* MethodSemantics */ {kU16, T(Table::MethodDef), C(HasSemantics)},
            /* MethodImpl */ {T(Table::TypeDef), C(MethodDefOrRef), C(MethodDefOrRef)},
            /* ModuleRef */ {kStr},
            /* TypeSpec */ {kBlob},
            /* ImplMap */ {kU16, C(MemberForwarded), kStr, T(Table::ModuleRef)},
            /* FieldRVA */ {kU32, T(Table::Field)},
            /* EncLog */ {kU32, kU32},
            /* EncMap */ {kU32},
            /* Assembly */ {kU32, kU16, kU16, kU16, kU16, kU32, kBlob, kStr, kStr},
            /* AssemblyProcessor */ {kU32},
            /* AssemblyOS */ {kU32, kU32, kU32},
            /* AssemblyRef */ {kU16, kU16, kU16, kU16, kU32, kBlob, kStr, kStr, kBlob},
            /* AssemblyRefProcessor */ {kU32, T(Table::AssemblyRef)},
            /* AssemblyRefOS */ {kU32, kU32, kU32, T(Table::AssemblyRef)},
            /* File */ {kU32, kStr, kBlob},
            /* ExportedType */ {kU32, kU32, kStr, kStr, C(Implementation)},
            /* ManifestResource */ {kU32, kU32, kStr, C(Implementation)},
            /* NestedClass */ {T(Table::TypeDef), T(Table::TypeDef)},
            /* GenericParam */ {kU16, kU16, C(TypeOrMethodDef), kStr},
            /* MethodSpec */ {C(MethodDefOrRef), kBlob},
            /* GenericParamConstraint */ {T(Table::GenericParam), C(TypeDefOrRef)},
        };

        u32 Read16(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8); }
        u32 Read32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }

        // Bounds checked view of a byte range.
        struct View
        {
            Span<const u8> bytes;
            bool Has(usize offset, usize count) const { return offset <= bytes.Size() && count <= bytes.Size() - offset; }
            u32 U16(usize offset) const { return Has(offset, 2) ? Read16(bytes.Data() + offset) : 0; }
            u32 U32(usize offset) const { return Has(offset, 4) ? Read32(bytes.Data() + offset) : 0; }
            Span<const u8> Sub(usize offset, usize count) const { return Has(offset, count) ? Span<const u8>(bytes.Data() + offset, count) : Span<const u8>(); }
        };

        Span<const u8> HeapBlob(Span<const u8> heap, u32 index)
        {
            usize at = index;
            u32 length = 0;
            if (!ReadCompressed(heap, at, length) || length > heap.Size() - at)
                return {};
            return Span<const u8>(heap.Data() + at, length);
        }
    }

    bool Assembly::Fail(const char* message)
    {
        error_ = message;
        return false;
    }

    usize Assembly::OffsetOfRva(u32 rva) const
    {
        for (const Section& s : sections_)
        {
            u32 size = Max(s.virtualSize, s.rawSize);
            if (rva >= s.virtualAddress && rva - s.virtualAddress < size)
            {
                usize offset = usize(s.rawOffset) + (rva - s.virtualAddress);
                return offset < image_.Size() ? offset : ~usize(0);
            }
        }
        return ~usize(0);
    }

    bool Assembly::Load(Span<const u8> image)
    {
        *this = Assembly();
        image_ = image;
        View file{image};
        if (file.U16(0) != 0x5A4D)
            return Fail("not a PE file");
        usize pe = file.U32(0x3C);
        if (file.U32(pe) != 0x00004550)
            return Fail("missing PE signature");
        u32 sectionCount = file.U16(pe + 6);
        u32 optionalSize = file.U16(pe + 20);
        usize optional = pe + 24;
        u32 magic = file.U16(optional);
        if (magic != 0x10B && magic != 0x20B)
            return Fail("unknown optional header");
        usize directories = optional + (magic == 0x10B ? 96 : 112);
        u32 directoryCount = file.U32(directories - 4);
        if (directoryCount < 15)
            return Fail("no CLI header");
        u32 cliRva = file.U32(directories + 14 * 8);
        usize sectionTable = optional + optionalSize;
        if (sectionCount > 96 || !file.Has(sectionTable, usize(sectionCount) * 40))
            return Fail("damaged section table");
        for (u32 i = 0; i < sectionCount; i++)
        {
            usize s = sectionTable + usize(i) * 40;
            sections_.Push({file.U32(s + 12), file.U32(s + 8), file.U32(s + 20), file.U32(s + 16)});
        }
        usize cli = OffsetOfRva(cliRva);
        if (cliRva == 0 || !file.Has(cli, 16))
            return Fail("not a managed assembly");
        usize root = OffsetOfRva(file.U32(cli + 8));
        u32 metadataSize = file.U32(cli + 12);
        if (!file.Has(root, metadataSize) || metadataSize < 20 || file.U32(root) != 0x424A5342)
            return Fail("damaged metadata root");
        View meta{file.Sub(root, metadataSize)};
        u32 versionLength = meta.U32(12);
        if (versionLength > 255)
            return Fail("damaged metadata version");
        usize at = 16 + usize(versionLength);
        u32 streamCount = meta.U16(at + 2);
        at += 4;
        Span<const u8> tableStream;
        for (u32 i = 0; i < streamCount; i++)
        {
            u32 offset = meta.U32(at), size = meta.U32(at + 4);
            at += 8;
            char name[32] = {};
            u32 n = 0;
            while (meta.Has(at + n, 1) && meta.bytes[at + n] != 0 && n < 31)
            {
                name[n] = char(meta.bytes[at + n]);
                n++;
            }
            at += (usize(n) + 4) & ~usize(3);
            Span<const u8> data = meta.Sub(offset, size);
            if (!std::strcmp(name, "#~") || !std::strcmp(name, "#-"))
                tableStream = data;
            else if (!std::strcmp(name, "#Strings"))
                strings_ = data;
            else if (!std::strcmp(name, "#US"))
                userStrings_ = data;
            else if (!std::strcmp(name, "#GUID"))
                guids_ = data;
            else if (!std::strcmp(name, "#Blob"))
                blobs_ = data;
        }
        if (tableStream.Empty())
            return Fail("no metadata tables");
        return ReadTables(tableStream);
    }

    bool Assembly::ReadTables(Span<const u8> stream)
    {
        View s{stream};
        if (!s.Has(0, 24))
            return Fail("damaged table stream");
        u8 heapSizes = stream[6];
        u64 valid = u64(s.U32(8)) | (u64(s.U32(12)) << 32);
        if (valid >> u32(Table::Count))
            return Fail("unknown metadata tables");
        usize at = 24;
        for (u32 t = 0; t < u32(Table::Count); t++)
        {
            if (!((valid >> t) & 1))
                continue;
            if (!s.Has(at, 4))
                return Fail("damaged table row counts");
            tables_[t].rows = s.U32(at);
            if (tables_[t].rows > 0xFFFFFF)
                return Fail("damaged table row counts");
            at += 4;
        }
        for (u32 t = 0; t < u32(Table::Count); t++)
        {
            TableLayout& layout = tables_[t];
            u32 offset = 0;
            for (u32 c = 0; c < 10 && kSchemas[t][c]; c++)
            {
                u8 kind = kSchemas[t][c];
                u32 size;
                if (kind == kU16)
                    size = 2;
                else if (kind == kU32)
                    size = 4;
                else if (kind == kStr)
                    size = heapSizes & 1 ? 4 : 2;
                else if (kind == kGuid)
                    size = heapSizes & 2 ? 4 : 2;
                else if (kind == kBlob)
                    size = heapSizes & 4 ? 4 : 2;
                else if (kind & kCoded)
                {
                    const CodedIndex& coded = kCodedIndexes[kind & 0x3F];
                    u32 maxRows = 0;
                    for (u32 i = 0; i < coded.count; i++)
                        if (coded.tables[i] != kNone)
                            maxRows = Max(maxRows, tables_[coded.tables[i]].rows);
                    size = maxRows < (1u << (16 - coded.bits)) ? 2 : 4;
                }
                else
                    size = tables_[kind & 0x3F].rows < 65536 ? 2 : 4;
                layout.offsets[c] = u8(offset);
                layout.sizes[c] = u8(size);
                layout.kinds[c] = kind;
                layout.columnCount = u8(c + 1);
                offset += size;
            }
            layout.rowSize = offset;
            usize bytes = usize(layout.rows) * layout.rowSize;
            if (!s.Has(at, bytes))
                return Fail("metadata tables exceed the stream");
            layout.data = stream.Data() + at;
            at += bytes;
        }
        return true;
    }

    u32 Assembly::Cell(Table table, u32 row, u32 column) const
    {
        const TableLayout& layout = tables_[usize(table)];
        if (row == 0 || row > layout.rows || column >= layout.columnCount)
            return 0;
        const u8* p = layout.data + usize(row - 1) * layout.rowSize + layout.offsets[column];
        return layout.sizes[column] == 2 ? Read16(p) : Read32(p);
    }

    Token Assembly::CodedCell(Table table, u32 row, u32 column) const
    {
        const TableLayout& layout = tables_[usize(table)];
        if (column >= layout.columnCount || !(layout.kinds[column] & kCoded))
            return {};
        const CodedIndex& coded = kCodedIndexes[layout.kinds[column] & 0x3F];
        u32 value = Cell(table, row, column);
        u32 tag = value & ((1u << coded.bits) - 1);
        if (tag >= coded.count || coded.tables[tag] == kNone)
            return {};
        return Token(Table(coded.tables[tag]), value >> coded.bits);
    }

    StringView Assembly::GetString(u32 index) const
    {
        if (index >= strings_.Size())
            return {};
        const char* start = reinterpret_cast<const char*>(strings_.Data()) + index;
        usize max = strings_.Size() - index, n = 0;
        while (n < max && start[n])
            n++;
        return n < max ? StringView(start, n) : StringView();
    }

    Span<const u8> Assembly::GetBlob(u32 index) const { return HeapBlob(blobs_, index); }
    Span<const u8> Assembly::GetUserString(u32 index) const
    {
        Span<const u8> blob = HeapBlob(userStrings_, index);
        return blob.Empty() ? blob : Span<const u8>(blob.Data(), blob.Size() & ~usize(1));
    }

    StringView Assembly::Name() const { return GetString(Cell(Table::Assembly, 1, 7)); }

    u32 Assembly::RangeEnd(Table owner, u32 row, u32 column, Table target) const
    {
        u32 limit = RowCount(target) + 1;
        u32 begin = Min(Cell(owner, row, column), limit);
        u32 end = row < RowCount(owner) ? Cell(owner, row + 1, column) : limit;
        return Max(begin, Min(end, limit));
    }

    u32 Assembly::OwnerOf(Table owner, u32 column, Table target, u32 row) const
    {
        if (row == 0 || row > RowCount(target))
            return 0;
        u32 low = 1, high = RowCount(owner), found = 0;
        while (low <= high)
        {
            u32 mid = low + (high - low) / 2;
            if (Cell(owner, mid, column) <= row)
            {
                found = mid;
                low = mid + 1;
            }
            else
                high = mid - 1;
        }
        return found;
    }

    TypeDefInfo Assembly::TypeDef(u32 row) const
    {
        TypeDefInfo t;
        if (row == 0 || row > RowCount(Table::TypeDef))
            return t;
        t.row = row;
        t.flags = Cell(Table::TypeDef, row, 0);
        t.name = GetString(Cell(Table::TypeDef, row, 1));
        t.nameSpace = GetString(Cell(Table::TypeDef, row, 2));
        t.extends = CodedCell(Table::TypeDef, row, 3);
        t.fieldBegin = Min(Cell(Table::TypeDef, row, 4), RowCount(Table::Field) + 1);
        t.fieldEnd = RangeEnd(Table::TypeDef, row, 4, Table::Field);
        t.methodBegin = Min(Cell(Table::TypeDef, row, 5), RowCount(Table::MethodDef) + 1);
        t.methodEnd = RangeEnd(Table::TypeDef, row, 5, Table::MethodDef);
        // NestedClass is sorted by the nested type.
        u32 low = 1, high = RowCount(Table::NestedClass);
        while (low <= high)
        {
            u32 mid = low + (high - low) / 2;
            u32 nested = Cell(Table::NestedClass, mid, 0);
            if (nested == row)
            {
                t.enclosingType = Cell(Table::NestedClass, mid, 1);
                break;
            }
            if (nested < row)
                low = mid + 1;
            else
                high = mid - 1;
        }
        return t;
    }

    FieldInfo Assembly::Field(u32 row) const
    {
        FieldInfo f;
        if (row == 0 || row > RowCount(Table::Field))
            return f;
        f.row = row;
        f.flags = Cell(Table::Field, row, 0);
        f.name = GetString(Cell(Table::Field, row, 1));
        f.signature = GetBlob(Cell(Table::Field, row, 2));
        return f;
    }

    MethodInfo Assembly::Method(u32 row) const
    {
        MethodInfo m;
        if (row == 0 || row > RowCount(Table::MethodDef))
            return m;
        m.row = row;
        m.rva = Cell(Table::MethodDef, row, 0);
        m.implFlags = Cell(Table::MethodDef, row, 1);
        m.flags = Cell(Table::MethodDef, row, 2);
        m.name = GetString(Cell(Table::MethodDef, row, 3));
        m.signature = GetBlob(Cell(Table::MethodDef, row, 4));
        m.paramBegin = Min(Cell(Table::MethodDef, row, 5), RowCount(Table::Param) + 1);
        m.paramEnd = RangeEnd(Table::MethodDef, row, 5, Table::Param);
        return m;
    }

    ParamInfo Assembly::Param(u32 row) const
    {
        ParamInfo p;
        if (row == 0 || row > RowCount(Table::Param))
            return p;
        p.row = row;
        p.flags = Cell(Table::Param, row, 0);
        p.sequence = Cell(Table::Param, row, 1);
        p.name = GetString(Cell(Table::Param, row, 2));
        return p;
    }

    MemberRefInfo Assembly::MemberRef(u32 row) const
    {
        MemberRefInfo m;
        m.parent = CodedCell(Table::MemberRef, row, 0);
        m.name = GetString(Cell(Table::MemberRef, row, 1));
        m.signature = GetBlob(Cell(Table::MemberRef, row, 2));
        return m;
    }

    u32 Assembly::FindType(StringView nameSpace, StringView name) const
    {
        for (u32 row = 1; row <= RowCount(Table::TypeDef); row++)
        {
            TypeDefInfo t = TypeDef(row);
            if (t.enclosingType == 0 && t.name == name && t.nameSpace == nameSpace)
                return row;
        }
        return 0;
    }

    u32 Assembly::FindNestedType(u32 enclosingRow, StringView name) const
    {
        for (u32 row = 1; row <= RowCount(Table::TypeDef); row++)
        {
            TypeDefInfo t = TypeDef(row);
            if (t.enclosingType == enclosingRow && t.name == name)
                return row;
        }
        return 0;
    }

    u32 Assembly::FindMethod(u32 typeRow, StringView name) const
    {
        TypeDefInfo t = TypeDef(typeRow);
        for (u32 row = t.methodBegin; row < t.methodEnd; row++)
            if (GetString(Cell(Table::MethodDef, row, 3)) == name)
                return row;
        return 0;
    }

    u32 Assembly::FindField(u32 typeRow, StringView name) const
    {
        TypeDefInfo t = TypeDef(typeRow);
        for (u32 row = t.fieldBegin; row < t.fieldEnd; row++)
            if (GetString(Cell(Table::Field, row, 1)) == name)
                return row;
        return 0;
    }

    u32 Assembly::DeclaringTypeOfMethod(u32 methodRow) const { return OwnerOf(Table::TypeDef, 5, Table::MethodDef, methodRow); }
    u32 Assembly::DeclaringTypeOfField(u32 fieldRow) const { return OwnerOf(Table::TypeDef, 4, Table::Field, fieldRow); }

    bool Assembly::TypeName(Token type, StringView& nameSpace, StringView& name) const
    {
        if (type.Is(Table::TypeDef) && type.Row() <= RowCount(Table::TypeDef))
        {
            name = GetString(Cell(Table::TypeDef, type.Row(), 1));
            nameSpace = GetString(Cell(Table::TypeDef, type.Row(), 2));
            return true;
        }
        if (type.Is(Table::TypeRef) && type.Row() <= RowCount(Table::TypeRef))
        {
            name = GetString(Cell(Table::TypeRef, type.Row(), 1));
            nameSpace = GetString(Cell(Table::TypeRef, type.Row(), 2));
            return true;
        }
        return false;
    }

    bool Assembly::Body(u32 methodRow, MethodBody& out) const
    {
        out = MethodBody();
        u32 rva = Cell(Table::MethodDef, methodRow, 0);
        usize at = rva ? OffsetOfRva(rva) : ~usize(0);
        View file{image_};
        if (!file.Has(at, 1))
            return false;
        u8 first = image_[at];
        if ((first & 3) == 2)
        {
            out.maxStack = 8;
            out.code = file.Sub(at + 1, first >> 2);
            return out.code.Size() == usize(first >> 2);
        }
        if ((first & 3) != 3 || !file.Has(at, 12))
            return false;
        u32 flags = file.U16(at);
        u32 headerSize = (flags >> 12) * 4;
        u32 codeSize = file.U32(at + 4);
        if (headerSize < 12)
            return false;
        out.maxStack = file.U16(at + 2);
        out.localsSignature = Token(file.U32(at + 8));
        out.initLocals = (flags & 0x10) != 0;
        out.hasExceptionRegions = (flags & 0x8) != 0;
        out.code = file.Sub(at + headerSize, codeSize);
        return out.code.Size() == codeSize;
    }

    Span<const u8> Assembly::TypeSpecSignature(u32 row) const { return GetBlob(Cell(Table::TypeSpec, row, 0)); }
    Span<const u8> Assembly::StandAloneSignature(u32 row) const { return GetBlob(Cell(Table::StandAloneSig, row, 0)); }

    void Assembly::Attributes(Token parent, Array<CustomAttribute>& out) const
    {
        const CodedIndex& coded = kCodedIndexes[HasCustomAttribute];
        u32 tag = coded.count;
        for (u32 i = 0; i < coded.count; i++)
            if (coded.tables[i] == u8(parent.TableOf()))
                tag = i;
        if (parent.IsNull() || tag == coded.count)
            return;
        // The table is sorted by the coded parent value.
        u32 key = (parent.Row() << coded.bits) | tag;
        u32 low = 1, high = RowCount(Table::CustomAttribute) + 1;
        while (low < high)
        {
            u32 mid = low + (high - low) / 2;
            if (Cell(Table::CustomAttribute, mid, 0) < key)
                low = mid + 1;
            else
                high = mid;
        }
        for (u32 row = low; row <= RowCount(Table::CustomAttribute) && Cell(Table::CustomAttribute, row, 0) == key; row++)
        {
            CustomAttribute a;
            a.constructor = CodedCell(Table::CustomAttribute, row, 1);
            a.value = GetBlob(Cell(Table::CustomAttribute, row, 2));
            Token type;
            if (a.constructor.Is(Table::MethodDef))
                type = Token(Table::TypeDef, DeclaringTypeOfMethod(a.constructor.Row()));
            else if (a.constructor.Is(Table::MemberRef))
                type = CodedCell(Table::MemberRef, a.constructor.Row(), 0);
            if (TypeName(type, a.typeNamespace, a.typeName))
                out.Push(a);
        }
    }
}
