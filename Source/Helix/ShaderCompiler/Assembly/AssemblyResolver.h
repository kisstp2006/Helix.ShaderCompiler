#pragma once

#include <Helix/ShaderCompiler/Assembly/Assembly.h>
#include <Helix/ShaderCompiler/Assembly/DebugInfo.h>

namespace helix::shadercompiler
{
    // Finds the assemblies a shader assembly references: by name, among assemblies added from memory and as name.dll in
    // the directories. Loaded images stay alive as long as the resolver; References() and Symbols() are what
    // TranslateOptions takes. Debug information comes from the PDB embedded in an assembly or from name.pdb next to it.
    class AssemblyResolver
    {
    public:
        AssemblyResolver() = default;
        AssemblyResolver(const AssemblyResolver&) = delete;
        AssemblyResolver& operator=(const AssemblyResolver&) = delete;
        ~AssemblyResolver();

        void AddDirectory(StringView directory);
        // An assembly that is already loaded; it must outlive the resolver.
        void Add(const Assembly& assembly);

        // Null when no added assembly has the name and no directory holds a loadable name.dll.
        const Assembly* Find(StringView name);

        // Finds every assembly `assembly` references, directly or through the ones found. Returns false when one is
        // missing; `missing` then names the first of them (references of the runtime, System.*, are not looked for).
        bool LoadReferences(const Assembly& assembly, String& missing);

        Span<const Assembly* const> References() const { return Span<const Assembly* const>(all_.Data(), all_.Size()); }
        Span<const DebugInfo* const> Symbols() const { return Span<const DebugInfo* const>(symbols_.Data(), symbols_.Size()); }

        // Looks for the debug information of an assembly (LoadReferences does it for the assemblies it walks).
        void LoadSymbols(const Assembly& assembly);

    private:
        struct Loaded
        {
            Array<u8> image;
            Assembly assembly;
        };

        struct LoadedSymbols
        {
            Array<u8> image;
            DebugInfo info;
        };

        Array<String> directories_;
        Array<Loaded*> owned_;
        Array<const Assembly*> all_;
        Array<LoadedSymbols*> ownedSymbols_;
        Array<const DebugInfo*> symbols_;
        Array<const Assembly*> withSymbols_;
    };
}
