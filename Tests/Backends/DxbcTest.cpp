#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Backends/Dxbc.h>
#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/Testing/Test.h>

#include <cstdio>
#include <cstring>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

namespace
{
    bool Compile(const Assembly& a, u32 method, Stage stage, DxbcShader& shader, String& error)
    {
        Module module;
        return TranslateEntryPoint(a, method, stage, module, error) && EmitDxbc(module, shader, error);
    }

    u32 U32(const Array<u8>& bytes, usize at)
    {
        u32 v = 0;
        if (at + 4 <= bytes.Size())
            std::memcpy(&v, bytes.Data() + at, 4);
        return v;
    }

    // Data of the chunk with the tag, empty when missing or out of bounds.
    Span<const u8> Chunk(const Array<u8>& bytes, const char* tag)
    {
        for (u32 i = 0; i < U32(bytes, 28); i++)
        {
            usize at = U32(bytes, 32 + usize(i) * 4);
            if (at + 8 <= bytes.Size() && std::memcmp(bytes.Data() + at, tag, 4) == 0 && at + 8 + U32(bytes, at + 4) <= bytes.Size())
                return {bytes.Data() + at + 8, U32(bytes, at + 4)};
        }
        return {};
    }

    bool HasElement(Span<const u8> signature, const char* name, u32 index, u32 reg)
    {
        Array<u8> copy;
        copy.Append(signature.Data(), signature.Size());
        for (u32 i = 0; i < U32(copy, 0); i++)
        {
            usize at = 8 + usize(i) * 24;
            usize nameAt = U32(copy, at);
            if (nameAt < copy.Size() && std::strcmp(reinterpret_cast<const char*>(copy.Data() + nameAt), name) == 0 && U32(copy, at + 4) == index &&
                U32(copy, at + 16) == reg)
                return true;
        }
        return false;
    }
}

HX_TEST(Dxbc, WritesAContainerWithSignatures)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    u32 forward = a.FindNestedType(a.FindType("Helix.ShaderTests", "Lit"), "Forward");
    DxbcShader vertex, fragment;
    String error;
    bool ok = Compile(a, a.FindMethod(forward, "Vertex"), Stage::Vertex, vertex, error) &&
              Compile(a, a.FindMethod(forward, "Fragment"), Stage::Fragment, fragment, error);
    if (!ok)
        std::printf("    %s\n", error.CStr());
    HX_REQUIRE(ok);

    for (const DxbcShader* s : {&vertex, &fragment})
    {
        HX_REQUIRE(s->bytes.Size() > 32u && std::memcmp(s->bytes.Data(), "DXBC", 4) == 0);
        HX_CHECK(U32(s->bytes, 20) == 1u && U32(s->bytes, 24) == s->bytes.Size() && U32(s->bytes, 28) == 3u);
        Span<const u8> code = Chunk(s->bytes, "SHDR");
        HX_REQUIRE(code.Size() >= 8u);
        u32 version, length;
        std::memcpy(&version, code.Data(), 4);
        std::memcpy(&length, code.Data() + 4, 4);
        HX_CHECK(version == (s == &vertex ? 0x00010040u : 0x00000040u) && usize(length) * 4 == code.Size());
    }
    // Stage outputs and inputs meet on the same registers.
    HX_CHECK(HasElement(Chunk(vertex.bytes, "ISGN"), "TEXCOORD", 0, 0) && HasElement(Chunk(vertex.bytes, "ISGN"), "TEXCOORD", 1, 1));
    HX_CHECK(HasElement(Chunk(vertex.bytes, "ISGN"), "SV_InstanceID", 0, 2));
    HX_CHECK(HasElement(Chunk(vertex.bytes, "OSGN"), "SV_Position", 0, 0) && HasElement(Chunk(vertex.bytes, "OSGN"), "TEXCOORD", 1, 2));
    HX_CHECK(HasElement(Chunk(fragment.bytes, "ISGN"), "SV_Position", 0, 0) && HasElement(Chunk(fragment.bytes, "ISGN"), "TEXCOORD", 1, 2));
    HX_CHECK(HasElement(Chunk(fragment.bytes, "OSGN"), "SV_Target", 0, 0));

    // The same module always gives the same bytes, checksum included.
    DxbcShader again;
    HX_REQUIRE(Compile(a, a.FindMethod(forward, "Fragment"), Stage::Fragment, again, error));
    HX_CHECK(again.bytes.Size() == fragment.bytes.Size() && std::memcmp(again.bytes.Data(), fragment.bytes.Data(), again.bytes.Size()) == 0);
    bool checksum = false;
    for (usize i = 4; i < 20; i++)
        checksum = checksum || fragment.bytes[i] != 0;
    HX_CHECK(checksum);
}

HX_TEST(Dxbc, RejectsWhatTheGpuCannotRun)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    u32 rejected = a.FindType("Helix.ShaderTests", "Rejected");
    HX_REQUIRE(rejected != 0);
    DxbcShader shader;
    String error;
    HX_CHECK(!Compile(a, a.FindMethod(rejected, "Layout"), Stage::Fragment, shader, error));
    std::printf("    %s\n", error.CStr());
    HX_CHECK(StringView(error).Contains("settings.direction") && StringView(error).Contains("byte 16"));
}
