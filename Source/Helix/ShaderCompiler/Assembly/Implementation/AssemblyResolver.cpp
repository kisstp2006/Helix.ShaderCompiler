#include <Helix/ShaderCompiler/Assembly/AssemblyResolver.h>

#include <Helix/Core/Strings/Format.h>

#include <cstdio>

namespace helix::shadercompiler
{
    namespace
    {
        bool ReadFile(const String& path, Array<u8>& out)
        {
            std::FILE* f = std::fopen(path.CStr(), "rb");
            if (!f)
                return false;
            bool ok = std::fseek(f, 0, SEEK_END) == 0;
            long size = ok ? std::ftell(f) : -1;
            ok = ok && size > 0 && std::fseek(f, 0, SEEK_SET) == 0;
            if (ok)
            {
                out.Resize(usize(size));
                ok = std::fread(out.Data(), 1, out.Size(), f) == out.Size();
            }
            std::fclose(f);
            return ok;
        }

        // Runtime assemblies hold no shader code; the frontend reports calls into them by name.
        bool IsRuntime(StringView name) { return name == "mscorlib" || name == "netstandard" || name == "System" || name.StartsWith("System."); }
    }

    AssemblyResolver::~AssemblyResolver()
    {
        for (Loaded* l : owned_)
            delete l;
    }

    void AssemblyResolver::AddDirectory(StringView directory) { directories_.Push(String(directory)); }

    void AssemblyResolver::Add(const Assembly& assembly)
    {
        for (const Assembly* a : all_)
            if (a == &assembly)
                return;
        all_.Push(&assembly);
    }

    const Assembly* AssemblyResolver::Find(StringView name)
    {
        for (const Assembly* a : all_)
            if (a->Name() == name)
                return a;
        for (const String& directory : directories_)
        {
            Loaded* l = new Loaded();
            if (ReadFile(Format("{}/{}.dll", directory, name), l->image) && l->assembly.Load(Span<const u8>(l->image.Data(), l->image.Size())) &&
                l->assembly.Name() == name)
            {
                owned_.Push(l);
                all_.Push(&l->assembly);
                return &l->assembly;
            }
            delete l;
        }
        return nullptr;
    }

    bool AssemblyResolver::LoadReferences(const Assembly& assembly, String& missing)
    {
        missing.Clear();
        Array<const Assembly*> work;
        work.Push(&assembly);
        while (!work.Empty())
        {
            const Assembly* a = work.Back();
            work.Pop();
            // Column 6 of AssemblyRef is the name.
            for (u32 row = 1; row <= a->RowCount(Table::AssemblyRef); row++)
            {
                StringView name = a->GetString(a->Cell(Table::AssemblyRef, row, 6));
                if (IsRuntime(name))
                    continue;
                bool known = false;
                for (const Assembly* k : all_)
                    known = known || k->Name() == name;
                if (known)
                    continue;
                if (const Assembly* found = Find(name))
                    work.Push(found);
                else if (missing.Empty())
                    missing = name;
            }
        }
        return missing.Empty();
    }
}
