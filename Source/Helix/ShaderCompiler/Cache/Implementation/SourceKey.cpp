#include <Helix/ShaderCompiler/Cache/ShaderCache.h>

#include <Helix/ShaderCompiler/Assembly/IL.h>
#include <Helix/ShaderCompiler/Assembly/Signature.h>
#include <Helix/ShaderCompiler/Cache/Implementation/Digest.h>

#include <cstring>

namespace helix::shadercompiler
{
    namespace
    {
        // Walks the metadata the translation of an entry point can read and feeds it to the digest, in the order it is
        // found, each item once.
        struct Closure
        {
            Array<const Assembly*> assemblies;
            Array<u64> seen;
            struct Item
            {
                u32 assembly;
                Token token;
            };
            Array<Item> work;
            cache::Digest d;

            void Push(u32 assembly, Token t)
            {
                if (t.IsNull())
                    return;
                u64 key = (u64(assembly) << 32) | t.value;
                if (seen.Contains(key))
                    return;
                seen.Push(key);
                work.Push({assembly, t});
            }

            void Blob(Span<const u8> b)
            {
                d.U64(b.Size());
                d.md5.Update(b.Data(), b.Size());
            }

            // The TypeDef a TypeRef stands for, among the assemblies; false for the runtime's own types.
            bool Resolve(u32 assembly, Token ref, u32& outAssembly, u32& outRow) const
            {
                const Assembly& a = *assemblies[assembly];
                StringView ns, name;
                if (!a.TypeName(ref, ns, name))
                    return false;
                Token scope = a.CodedCell(Table::TypeRef, ref.Row(), 0);
                if (scope.Is(Table::TypeRef))
                {
                    u32 outer = 0;
                    if (!Resolve(assembly, scope, outAssembly, outer))
                        return false;
                    outRow = assemblies[outAssembly]->FindNestedType(outer, name);
                    return outRow != 0;
                }
                if (!scope.Is(Table::AssemblyRef))
                    return false;
                StringView target = a.GetString(a.Cell(Table::AssemblyRef, scope.Row(), 6));
                for (usize i = 0; i < assemblies.Size(); i++)
                    if (assemblies[i]->Name() == target)
                    {
                        outAssembly = u32(i);
                        outRow = assemblies[i]->FindType(ns, name);
                        return outRow != 0;
                    }
                return false;
            }

            void Types(u32 assembly, const TypeSig& sig)
            {
                if (!sig.token.IsNull())
                    Push(assembly, sig.token);
                for (const TypeSig& arg : sig.args)
                    Types(assembly, arg);
            }

            void Attributes(u32 assembly, Token parent)
            {
                Array<CustomAttribute> list;
                assemblies[assembly]->Attributes(parent, list);
                d.U64(list.Size());
                for (const CustomAttribute& c : list)
                {
                    d.Text(c.typeNamespace);
                    d.Text(c.typeName);
                    Blob(c.value);
                }
            }

            void Method(u32 assembly, u32 row)
            {
                const Assembly& a = *assemblies[assembly];
                MethodInfo info = a.Method(row);
                d.Text(info.name);
                d.U64(u64(info.flags) | (u64(info.implFlags) << 32));
                Blob(info.signature);
                MethodSig sig;
                if (ParseMethodSignature(info.signature, sig))
                {
                    Types(assembly, sig.returnType);
                    for (const TypeSig& p : sig.params)
                        Types(assembly, p);
                }
                Attributes(assembly, Token(Table::MethodDef, row));
                for (u32 p = info.paramBegin; p < info.paramEnd; p++)
                {
                    ParamInfo param = a.Param(p);
                    d.Text(param.name);
                    d.U64(u64(param.sequence) | (u64(param.flags) << 32));
                    Attributes(assembly, Token(Table::Param, p));
                }
                Push(assembly, Token(Table::TypeDef, a.DeclaringTypeOfMethod(row)));
                MethodBody body;
                if (!a.Body(row, body))
                {
                    d.U64(0);
                    return;
                }
                Blob(body.code);
                d.U64(u64(body.hasExceptionRegions));
                if (body.localsSignature.Is(Table::StandAloneSig))
                {
                    Span<const u8> locals = a.StandAloneSignature(body.localsSignature.Row());
                    Blob(locals);
                    Array<TypeSig> types;
                    if (ParseLocalsSignature(locals, types))
                        for (const TypeSig& t : types)
                            Types(assembly, t);
                }
                // Every token an instruction names is part of what the method reads.
                Instruction in;
                for (u32 at = 0; at < body.code.Size();)
                {
                    if (!DecodeInstruction(body.code, at, in))
                        break;
                    if (in.operand == OperandKind::Token)
                    {
                        Token t(u32(in.value));
                        if (t.Is(Table::MethodDef) || t.Is(Table::MemberRef) || t.Is(Table::MethodSpec) || t.Is(Table::Field) || t.Is(Table::TypeDef) ||
                            t.Is(Table::TypeRef) || t.Is(Table::TypeSpec) || t.Is(Table::StandAloneSig))
                            Push(assembly, t);
                    }
                }
            }

            void Type(u32 assembly, u32 row)
            {
                const Assembly& a = *assemblies[assembly];
                TypeDefInfo t = a.TypeDef(row);
                d.Text(t.nameSpace);
                d.Text(t.name);
                d.U64(t.flags);
                Push(assembly, t.extends);
                if (t.enclosingType)
                    Push(assembly, Token(Table::TypeDef, t.enclosingType));
                Attributes(assembly, Token(Table::TypeDef, row));
                for (u32 f = t.fieldBegin; f < t.fieldEnd; f++)
                {
                    FieldInfo field = a.Field(f);
                    d.Text(field.name);
                    d.U64(field.flags);
                    Blob(field.signature);
                    TypeSig sig;
                    if (ParseFieldSignature(field.signature, sig))
                        Types(assembly, sig);
                    Attributes(assembly, Token(Table::Field, f));
                    // Enum members and other literals carry their value in the Constant table.
                    if (field.IsLiteral())
                        for (u32 c = 1; c <= a.RowCount(Table::Constant); c++)
                            if (a.CodedCell(Table::Constant, c, 1) == Token(Table::Field, f))
                                Blob(a.GetBlob(a.Cell(Table::Constant, c, 2)));
                }
            }

            void Visit(const Item& item)
            {
                const Assembly& a = *assemblies[item.assembly];
                Token t = item.token;
                d.U64(t.value);
                d.Text(a.Name());
                if (t.Is(Table::MethodDef))
                    Method(item.assembly, t.Row());
                else if (t.Is(Table::TypeDef))
                    Type(item.assembly, t.Row());
                else if (t.Is(Table::Field))
                    Push(item.assembly, Token(Table::TypeDef, a.DeclaringTypeOfField(t.Row())));
                else if (t.Is(Table::TypeRef))
                {
                    u32 assembly = 0, row = 0;
                    StringView ns, name;
                    a.TypeName(t, ns, name);
                    d.Text(ns);
                    d.Text(name);
                    if (Resolve(item.assembly, t, assembly, row))
                        Push(assembly, Token(Table::TypeDef, row));
                }
                else if (t.Is(Table::TypeSpec))
                {
                    Span<const u8> blob = a.TypeSpecSignature(t.Row());
                    Blob(blob);
                    TypeSig sig;
                    if (ParseTypeSpec(blob, sig))
                        Types(item.assembly, sig);
                }
                else if (t.Is(Table::MethodSpec))
                {
                    Span<const u8> blob = a.GetBlob(a.Cell(Table::MethodSpec, t.Row(), 1));
                    Blob(blob);
                    Array<TypeSig> args;
                    if (ParseMethodSpec(blob, args))
                        for (const TypeSig& arg : args)
                            Types(item.assembly, arg);
                    Push(item.assembly, a.CodedCell(Table::MethodSpec, t.Row(), 0));
                }
                else if (t.Is(Table::StandAloneSig))
                    Blob(a.StandAloneSignature(t.Row()));
                else if (t.Is(Table::MemberRef))
                {
                    // A member of another type: its name and signature, the type, and the methods of that name there.
                    MemberRefInfo ref = a.MemberRef(t.Row());
                    d.Text(ref.name);
                    Blob(ref.signature);
                    Push(item.assembly, ref.parent);
                    u32 assembly = item.assembly, row = 0;
                    if (ref.parent.Is(Table::TypeRef) && Resolve(item.assembly, ref.parent, assembly, row))
                    {
                        TypeDefInfo type = assemblies[assembly]->TypeDef(row);
                        for (u32 m = type.methodBegin; m < type.methodEnd; m++)
                            if (assemblies[assembly]->Method(m).name == ref.name)
                                Push(assembly, Token(Table::MethodDef, m));
                    }
                }
            }
        };
    }

    ShaderKey MakeSourceKey(const Assembly& assembly, u32 method, Stage stage, const TranslateOptions& options, ShaderTarget target)
    {
        Closure c;
        c.assemblies.Push(&assembly);
        for (const Assembly* r : options.references)
            if (r && r != &assembly)
                c.assemblies.Push(r);
        c.d.U64(u64(kShaderCompilerVersion) | (u64(target) << 32) | (u64(stage) << 40) | (u64(options.passes.optimize) << 48) | (u64(1) << 56));
        // Keyword values in name order, so the order they are given in does not matter.
        Array<usize> order;
        for (usize i = 0; i < options.keywords.Size(); i++)
        {
            usize at = order.Size();
            while (at > 0 && options.keywords[i].name < StringView(options.keywords[order[at - 1]].name))
                at--;
            order.Insert(at, i);
        }
        c.d.U64(order.Size());
        for (usize i : order)
        {
            c.d.Text(options.keywords[i].name);
            c.d.U64(u64(options.keywords[i].value));
        }
        c.Push(0, Token(Table::MethodDef, method));
        for (usize i = 0; i < c.work.Size(); i++)
            c.Visit(c.work[i]);
        Md5Digest digest = c.d.md5.Finish();
        ShaderKey key;
        std::memcpy(key.bytes, digest.bytes, 16);
        return key;
    }

    ShaderKey CombineKeys(const ShaderKey& first, const ShaderKey& second)
    {
        Md5 md5;
        md5.Update(first.bytes, 16);
        md5.Update(second.bytes, 16);
        Md5Digest digest = md5.Finish();
        ShaderKey key;
        std::memcpy(key.bytes, digest.bytes, 16);
        return key;
    }

    bool CompileEntryPoint(const Assembly& assembly, u32 method, Stage stage, const TranslateOptions& options, ShaderTarget target, CompiledShader& out,
                           String& error, ShaderCache* cache)
    {
        error.Clear();
        ShaderKey source;
        if (cache)
        {
            source = MakeSourceKey(assembly, method, stage, options, target);
            if (cache->Find(source, out))
                return true;
        }
        Module module;
        if (!TranslateEntryPoint(assembly, method, stage, module, error, options) || !CompileModule(module, target, out, error, cache))
            return false;
        if (cache)
            cache->Store(source, out);
        return true;
    }
}
