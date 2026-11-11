#pragma once

#include <Helix/Core/Containers/HashMap.h>
#include <Helix/ShaderCompiler/Backends/Glsl.h>
#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/ShaderCompiler/IR/IR.h>

#include <atomic>
#include <mutex>

namespace helix::shadercompiler
{
    // Part of every cache key. Raise it whenever a backend changes what it writes for the same module, so results of
    // older builds on disk are not used.
    inline constexpr u32 kShaderCompilerVersion = 2;

    enum class ShaderTarget : u8
    {
        Glsl330,
        Glsl450,
        Essl300,
        Glsl430,
        Essl310,
        Dxbc,
        Essl320,
    };

    // One compiled shader: GLSL text or a Direct3D bytecode container.
    struct CompiledShader
    {
        ShaderTarget target = ShaderTarget::Glsl330;
        Stage stage = Stage::Vertex;
        Array<u8> code;
        // GLSL only: names in the source and the slots they stand for.
        Array<GlslBinding> bindings;
        u32 groupSize[3] = {1, 1, 1};

        StringView Text() const { return StringView(reinterpret_cast<const char*>(code.Data()), code.Size()); }
    };

    // Identifies a compilation by its content: the translated module (so only code the shader reaches, after the
    // keywords of the variant were applied), the target and the compiler version. Variants that end up with the same
    // code share one key.
    struct ShaderKey
    {
        u8 bytes[16] = {};

        bool operator==(const ShaderKey&) const = default;
    };

    ShaderKey MakeShaderKey(const Module& module, ShaderTarget target);

    // The first level key: everything the translation of the entry point reads, so the frontend can be skipped. That is
    // the closure reachable from the method (methods with their bodies, signatures, parameters and attributes; the
    // types, fields, enums and resources they use) in the shader assembly and the referenced ones, with the stage, the
    // keyword values, the optimization switch, the target and the compiler version. Code outside the closure does not
    // count, so changing it keeps every shader that does not reach it.
    ShaderKey MakeSourceKey(const Assembly& assembly, u32 method, Stage stage, const TranslateOptions& options, ShaderTarget target);

    // A key for two keys in this order (a vertex stage linked to its fragment stage).
    ShaderKey CombineKeys(const ShaderKey& first, const ShaderKey& second);

    // Keeps compiled shaders by key, in memory and optionally as one file per shader in a directory. Thread safe: one
    // cache serves every thread that compiles.
    class ShaderCache
    {
    public:
        // With a directory (which must exist) results survive the process; unreadable or damaged files count as missing.
        explicit ShaderCache(StringView directory = {});

        bool Find(const ShaderKey& key, CompiledShader& out);
        void Store(const ShaderKey& key, const CompiledShader& shader);

        // How often Find gave a result from memory, from a file, or nothing.
        u32 MemoryHits() const { return memoryHits_.load(); }
        u32 FileHits() const { return fileHits_.load(); }
        u32 Misses() const { return misses_.load(); }

    private:
        String Path(const ShaderKey& key) const;

        String directory_;
        HashMap<ShaderKey, CompiledShader> entries_;
        std::mutex mutex_;
        std::atomic<u32> memoryHits_{0}, fileHits_{0}, misses_{0};
    };

    // Runs the backend of the target on the module. With a cache, a module compiled before is not compiled again.
    bool CompileModule(const Module& module, ShaderTarget target, CompiledShader& out, String& error, ShaderCache* cache = nullptr);

    // Translates and compiles an entry point through both levels of the cache: a hit on the source key skips the
    // frontend, a hit on the module key skips the backend.
    bool CompileEntryPoint(const Assembly& assembly, u32 method, Stage stage, const TranslateOptions& options, ShaderTarget target, CompiledShader& out,
                           String& error, ShaderCache* cache = nullptr);
}

namespace helix
{
    template <>
    struct Hasher<shadercompiler::ShaderKey>
    {
        u64 operator()(const shadercompiler::ShaderKey& key) const
        {
            u64 value = 0;
            for (int i = 0; i < 8; i++)
                value |= u64(key.bytes[i]) << (i * 8);
            return value;
        }
    };
}
