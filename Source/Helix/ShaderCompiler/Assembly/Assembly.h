#pragma once

#include <Helix/Core/Basics/Span.h>
#include <Helix/Core/Containers/Array.h>
#include <Helix/Core/Strings/String.h>
#include <Helix/Core/Strings/StringView.h>

namespace helix::shadercompiler
{
    // Metadata tables of ECMA-335 II.22, in file order.
    enum class Table : u8
    {
        Module, TypeRef, TypeDef, FieldPtr, Field, MethodPtr, MethodDef, ParamPtr, Param, InterfaceImpl, MemberRef, Constant,
        CustomAttribute, FieldMarshal, DeclSecurity, ClassLayout, FieldLayout, StandAloneSig, EventMap, EventPtr, Event,
        PropertyMap, PropertyPtr, Property, MethodSemantics, MethodImpl, ModuleRef, TypeSpec, ImplMap, FieldRVA, EncLog,
        EncMap, Assembly, AssemblyProcessor, AssemblyOS, AssemblyRef, AssemblyRefProcessor, AssemblyRefOS, File,
        ExportedType, ManifestResource, NestedClass, GenericParam, MethodSpec, GenericParamConstraint,
        Count,
    };

    // A metadata token: table in the top byte, 1 based row below; 0 means none.
    struct Token
    {
        u32 value = 0;
        constexpr Token() = default;
        constexpr explicit Token(u32 v) : value(v) {}
        constexpr Token(Table table, u32 row) : value(row ? (u32(table) << 24) | row : 0) {}
        constexpr Table TableOf() const { return Table(value >> 24); }
        constexpr u32 Row() const { return value & 0xFFFFFF; }
        constexpr bool IsNull() const { return Row() == 0; }
        constexpr bool Is(Table table) const { return !IsNull() && TableOf() == table; }
        constexpr bool operator==(const Token&) const = default;
    };

    struct TypeDefInfo
    {
        u32 row = 0;
        StringView name, nameSpace;
        u32 flags = 0;
        Token extends;
        u32 fieldBegin = 0, fieldEnd = 0;
        u32 methodBegin = 0, methodEnd = 0;
        u32 enclosingType = 0;
    };

    struct FieldInfo
    {
        u32 row = 0;
        StringView name;
        u32 flags = 0;
        Span<const u8> signature;
        bool IsStatic() const { return (flags & 0x10) != 0; }
        bool IsInitOnly() const { return (flags & 0x20) != 0; }
        bool IsLiteral() const { return (flags & 0x40) != 0; }
    };

    struct MethodInfo
    {
        u32 row = 0;
        StringView name;
        u32 flags = 0, implFlags = 0, rva = 0;
        Span<const u8> signature;
        u32 paramBegin = 0, paramEnd = 0;
        bool IsStatic() const { return (flags & 0x10) != 0; }
    };

    struct ParamInfo
    {
        u32 row = 0;
        StringView name;
        // 0 is the return value, 1 the first parameter.
        u32 sequence = 0;
        u32 flags = 0;
    };

    struct MemberRefInfo
    {
        Token parent;
        StringView name;
        Span<const u8> signature;
    };

    struct MethodBody
    {
        Span<const u8> code;
        u32 maxStack = 0;
        Token localsSignature;
        bool initLocals = false;
        bool hasExceptionRegions = false;
    };

    struct CustomAttribute
    {
        StringView typeName, typeNamespace;
        // The constructor: a MethodDef or MemberRef token.
        Token constructor;
        Span<const u8> value;
    };

    // A loaded managed assembly (PE file with CLI metadata). Every access is bounds checked; damaged files fail to load
    // or return empty values, they never read outside the image. The image must stay alive while the object is used.
    class Assembly
    {
    public:
        bool Load(Span<const u8> image);
        const String& Error() const { return error_; }

        u32 RowCount(Table table) const { return tables_[usize(table)].rows; }
        // Raw cell of a table; rows are 1 based. Index columns are returned as stored (not decoded).
        u32 Cell(Table table, u32 row, u32 column) const;
        // Cell of a coded index column decoded to a token.
        Token CodedCell(Table table, u32 row, u32 column) const;

        StringView GetString(u32 index) const;
        Span<const u8> GetBlob(u32 index) const;
        // #US heap entry as UTF-16 code units (little endian bytes).
        Span<const u8> GetUserString(u32 index) const;

        StringView Name() const;
        TypeDefInfo TypeDef(u32 row) const;
        FieldInfo Field(u32 row) const;
        MethodInfo Method(u32 row) const;
        ParamInfo Param(u32 row) const;
        MemberRefInfo MemberRef(u32 row) const;

        // 0 when not found. Nested types are found through their enclosing type.
        u32 FindType(StringView nameSpace, StringView name) const;
        u32 FindNestedType(u32 enclosingRow, StringView name) const;
        u32 FindMethod(u32 typeRow, StringView name) const;
        u32 FindField(u32 typeRow, StringView name) const;
        u32 DeclaringTypeOfMethod(u32 methodRow) const;
        u32 DeclaringTypeOfField(u32 fieldRow) const;

        // Namespace and name of a TypeDef or TypeRef token; false for other tokens.
        bool TypeName(Token type, StringView& nameSpace, StringView& name) const;

        bool Body(u32 methodRow, MethodBody& out) const;
        Span<const u8> TypeSpecSignature(u32 row) const;
        Span<const u8> StandAloneSignature(u32 row) const;

        // Appends the custom attributes attached to a token (type, method, field, parameter, ...).
        void Attributes(Token parent, Array<CustomAttribute>& out) const;

    private:
        struct TableLayout
        {
            const u8* data = nullptr;
            u32 rows = 0;
            u32 rowSize = 0;
            u8 columnCount = 0;
            u8 offsets[12] = {};
            u8 sizes[12] = {};
            u8 kinds[12] = {};
        };

        bool Fail(const char* message);
        bool ReadTables(Span<const u8> stream);
        usize OffsetOfRva(u32 rva) const;
        u32 RangeEnd(Table owner, u32 row, u32 column, Table target) const;
        u32 OwnerOf(Table owner, u32 column, Table target, u32 row) const;

        struct Section
        {
            u32 virtualAddress, virtualSize, rawOffset, rawSize;
        };

        Span<const u8> image_;
        Array<Section> sections_;
        Span<const u8> strings_, blobs_, userStrings_, guids_;
        TableLayout tables_[usize(Table::Count)];
        String error_;
    };
}
