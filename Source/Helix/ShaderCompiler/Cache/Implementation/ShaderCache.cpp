#include <Helix/ShaderCompiler/Cache/ShaderCache.h>

#include <Helix/Core/Hashing/Hash.h>
#include <Helix/Core/Hashing/Md5.h>
#include <Helix/Core/Strings/Format.h>
#include <Helix/ShaderCompiler/Backends/Dxbc.h>
#include <Helix/ShaderCompiler/Cache/Implementation/Digest.h>

#include <cstdio>
#include <cstring>

namespace helix::shadercompiler
{
    using cache::Digest;

    namespace
    {
        void Put(Array<u8>& out, const void* data, usize size) { out.Append(static_cast<const u8*>(data), size); }
        void PutU32(Array<u8>& out, u32 v) { Put(out, &v, 4); }

        constexpr u32 kMagic = 0x43535848; // "HXSC"

        // File: magic, compiler version, target, stage, 3 group sizes, code size, code, binding count
        // { name size, name, kind, slot }, then a 64 bit hash of everything before it.
        Array<u8> Serialize(const CompiledShader& s)
        {
            Array<u8> out;
            PutU32(out, kMagic);
            PutU32(out, kShaderCompilerVersion);
            PutU32(out, u32(s.target));
            PutU32(out, u32(s.stage));
            for (u32 size : s.groupSize)
                PutU32(out, size);
            PutU32(out, u32(s.code.Size()));
            Put(out, s.code.Data(), s.code.Size());
            PutU32(out, u32(s.bindings.Size()));
            for (const GlslBinding& b : s.bindings)
            {
                PutU32(out, u32(b.name.Size()));
                Put(out, b.name.Data(), b.name.Size());
                PutU32(out, u32(b.kind));
                PutU32(out, b.slot);
            }
            u64 check = Hash64(out.Data(), out.Size());
            Put(out, &check, 8);
            return out;
        }

        bool Deserialize(const Array<u8>& data, CompiledShader& s)
        {
            if (data.Size() < 16)
                return false;
            usize end = data.Size() - 8, at = 0;
            u64 check;
            std::memcpy(&check, data.Data() + end, 8);
            if (check != Hash64(data.Data(), end))
                return false;
            bool ok = true;
            auto u32At = [&]() {
                u32 v = 0;
                if (at + 4 <= end)
                    std::memcpy(&v, data.Data() + at, 4);
                else
                    ok = false;
                at += 4;
                return v;
            };
            auto bytes = [&](usize count) -> const u8* {
                const u8* p = at + count <= end ? data.Data() + at : nullptr;
                ok = ok && p;
                at += count;
                return p;
            };
            if (u32At() != kMagic || u32At() != kShaderCompilerVersion)
                return false;
            s = CompiledShader();
            s.target = ShaderTarget(u32At());
            s.stage = Stage(u32At());
            for (u32& size : s.groupSize)
                size = u32At();
            u32 codeSize = u32At();
            if (const u8* code = bytes(codeSize))
                s.code.Append(code, codeSize);
            u32 bindingCount = u32At();
            for (u32 i = 0; i < bindingCount && ok; i++)
            {
                GlslBinding b;
                u32 nameSize = u32At();
                if (const u8* name = bytes(nameSize))
                    b.name = StringView(reinterpret_cast<const char*>(name), nameSize);
                b.kind = GlobalKind(u32At());
                b.slot = u32At();
                s.bindings.Push(Move(b));
            }
            return ok && at == end;
        }
    }

    ShaderKey MakeShaderKey(const Module& m, ShaderTarget target)
    {
        Digest d;
        d.U64(u64(kShaderCompilerVersion) | (u64(target) << 32) | (u64(m.stage) << 40));
        d.U64(u64(m.entry) | (u64(m.returnSemantic) << 32) | (u64(m.returnSemanticIndex) << 40));
        d.U64(u64(m.groupSize[0]) | (u64(m.groupSize[1]) << 32));
        d.U64(m.groupSize[2]);
        d.U64(m.structs.Size());
        for (const StructType& s : m.structs)
        {
            d.Text(s.name);
            d.U64(s.fields.Size());
            for (const StructField& f : s.fields)
            {
                d.Text(f.name);
                d.Add(f.type);
                d.U64(u64(f.semantic) | (u64(f.flat) << 8) | (u64(f.unread) << 16) | (u64(f.semanticIndex) << 32));
            }
        }
        d.U64(m.arrays.Size());
        for (const ArrayType& a : m.arrays)
        {
            d.Add(a.element);
            d.U64(a.length);
        }
        d.U64(m.globals.Size());
        for (const Global& g : m.globals)
        {
            d.Text(g.name);
            d.Add(g.type);
            d.Add(g.element);
            d.U64(u64(g.kind) | (u64(g.writable) << 8) | (u64(g.format) << 16) | (u64(g.comparison) << 24) | (u64(g.readable) << 25) | (u64(g.slot) << 32));
        }
        d.U64(m.functions.Size());
        for (const Function& f : m.functions)
            d.Add(f);
        Md5Digest digest = d.md5.Finish();
        ShaderKey key;
        std::memcpy(key.bytes, digest.bytes, 16);
        return key;
    }

    ShaderCache::ShaderCache(StringView directory) : directory_(directory) {}

    String ShaderCache::Path(const ShaderKey& key) const
    {
        String path = directory_;
        path.Append('/');
        for (u8 byte : key.bytes)
        {
            path.Append("0123456789abcdef"[byte >> 4]);
            path.Append("0123456789abcdef"[byte & 15]);
        }
        path.Append(".hxsc");
        return path;
    }

    bool ShaderCache::Find(const ShaderKey& key, CompiledShader& out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const CompiledShader* found = entries_.Find(key))
        {
            out = *found;
            memoryHits_++;
            return true;
        }
        if (!directory_.Empty())
        {
            if (std::FILE* f = std::fopen(Path(key).CStr(), "rb"))
            {
                Array<u8> data;
                u8 buffer[16384];
                for (usize n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;)
                    data.Append(buffer, n);
                std::fclose(f);
                if (Deserialize(data, out))
                {
                    entries_.Set(key, out);
                    fileHits_++;
                    return true;
                }
            }
        }
        misses_++;
        return false;
    }

    void ShaderCache::Store(const ShaderKey& key, const CompiledShader& shader)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.Set(key, shader);
        if (directory_.Empty())
            return;
        // Written under another name first, so a reader never sees half a file.
        String path = Path(key), temporary = Format("{}.tmp", path);
        std::FILE* f = std::fopen(temporary.CStr(), "wb");
        if (!f)
            return;
        Array<u8> data = Serialize(shader);
        bool written = std::fwrite(data.Data(), 1, data.Size(), f) == data.Size();
        std::fclose(f);
        std::remove(path.CStr());
        if (!written || std::rename(temporary.CStr(), path.CStr()) != 0)
            std::remove(temporary.CStr());
    }

    bool CompileModule(const Module& module, ShaderTarget target, CompiledShader& out, String& error, ShaderCache* cache)
    {
        error.Clear();
        ShaderKey key;
        if (cache)
        {
            key = MakeShaderKey(module, target);
            if (cache->Find(key, out))
                return true;
        }
        out = CompiledShader();
        out.target = target;
        out.stage = module.stage;
        for (int i = 0; i < 3; i++)
            out.groupSize[i] = module.groupSize[i];
        if (target == ShaderTarget::Dxbc)
        {
            DxbcShader shader;
            if (!EmitDxbc(module, shader, error))
                return false;
            out.code = Move(shader.bytes);
        }
        else
        {
            static constexpr GlslTarget kTargets[] = {GlslTarget::Glsl330, GlslTarget::Glsl450, GlslTarget::Essl300, GlslTarget::Glsl430,
                                                      GlslTarget::Essl310, GlslTarget::Glsl330, GlslTarget::Essl320};
            GlslShader shader;
            if (!EmitGlsl(module, kTargets[u32(target)], shader, error))
                return false;
            out.code.Append(reinterpret_cast<const u8*>(shader.source.Data()), shader.source.Size());
            out.bindings = Move(shader.bindings);
        }
        if (cache)
            cache->Store(key, out);
        return true;
    }
}
