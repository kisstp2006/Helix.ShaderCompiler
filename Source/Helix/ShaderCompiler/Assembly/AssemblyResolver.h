#pragma once

#include <Helix/ShaderCompiler/Assembly/Assembly.h>

namespace helix::shadercompiler
{
    // Finds the assemblies a shader assembly references: by name, among assemblies added from memory and as name.dll in
    // the directories. Loaded images stay alive as long as the resolver; References() is what TranslateOptions takes.
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

    private:
        struct Loaded
        {
            Array<u8> image;
            Assembly assembly;
        };

        Array<String> directories_;
        Array<Loaded*> owned_;
        Array<const Assembly*> all_;
    };
}
