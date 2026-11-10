#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Backends/Dxbc.h>
#include <Helix/ShaderCompiler/Backends/Glsl.h>
#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/Testing/Test.h>

#include <cstdio>
#include <cstring>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

namespace
{
    u32 Word(const Array<u8>& b, usize at)
    {
        u32 v = 0;
        if (at + 4 <= b.Size())
            std::memcpy(&v, b.Data() + at, 4);
        return v;
    }

    // Instructions of the shader chunk of a bytecode container, declarations not counted.
    u32 Instructions(const Array<u8>& code)
    {
        u32 chunks = Word(code, 28);
        for (u32 c = 0; c < chunks; c++)
        {
            usize chunk = Word(code, 32 + usize(c) * 4);
            u32 tag = Word(code, chunk);
            if (tag != 0x52444853u && tag != 0x58454853u) // SHDR, SHEX
                continue;
            usize at = chunk + 8, end = at + usize(Word(code, at + 4)) * 4;
            at += 8;
            u32 count = 0;
            while (at < end)
            {
                u32 token = Word(code, at);
                u32 opcode = token & 0x7FF;
                // Custom data (immediate constants) holds its length in the next word.
                u32 length = opcode == 53 ? Word(code, at + 4) : (token >> 24) & 0x7F;
                if (length == 0)
                    break;
                bool declaration = (opcode >= 88 && opcode <= 106) || (opcode >= 143 && opcode <= 162) || opcode == 53;
                count += declaration ? 0 : 1;
                at += usize(length) * 4;
            }
            return count;
        }
        return 0;
    }

    struct Size
    {
        u32 instructions = 0;
        usize glsl = 0;
    };

    bool Measure(const Assembly& a, u32 method, Stage stage, bool optimize, Span<const Assembly* const> references, Size& out)
    {
        TranslateOptions options;
        options.references = references;
        options.passes.optimize = optimize;
        Module module;
        DxbcShader code;
        GlslShader glsl;
        String error;
        bool ok = TranslateEntryPoint(a, method, stage, module, error, options) && EmitDxbc(module, code, error) &&
                  EmitGlsl(module, stage == Stage::Compute ? GlslTarget::Glsl430 : GlslTarget::Glsl450, glsl, error);
        if (!ok)
            return false;
        out.instructions = Instructions(code.bytes);
        out.glsl = glsl.source.Size();
        return true;
    }
}

HX_TEST(Passes, MeasureEveryCaseAndKernel)
{
    Assembly a, library, includes;
    HX_REQUIRE(a.Load(TestShadersImage()) && library.Load(ShaderLibraryImage()) && includes.Load(ShaderIncludesImage()));
    const Assembly* list[] = {&library, &includes};
    Span<const Assembly* const> references(list, 2);
    Size totalBefore, totalAfter;
    u32 measured = 0;
    std::printf("    %-20s %20s %20s\n", "", "instructions", "GLSL bytes");
    for (u32 row = 1; row <= a.RowCount(Table::TypeDef); row++)
    {
        TypeDefInfo type = a.TypeDef(row);
        bool kernel = type.nameSpace == StringView("Helix.ShaderTests.Kernels");
        if (!kernel && type.nameSpace != StringView("Helix.ShaderTests.Cases"))
            continue;
        u32 method = a.FindMethod(row, kernel ? "Run" : "Fragment");
        if (!method)
            continue;
        Size before, after;
        Stage stage = kernel ? Stage::Compute : Stage::Fragment;
        if (!Measure(a, method, stage, false, references, before) || !Measure(a, method, stage, true, references, after))
            continue;
        measured++;
        std::printf("    %-20s %9u -> %-8u %9zu -> %-8zu\n", String(type.name).CStr(), before.instructions, after.instructions, before.glsl, after.glsl);
        totalBefore.instructions += before.instructions;
        totalAfter.instructions += after.instructions;
        totalBefore.glsl += before.glsl;
        totalAfter.glsl += after.glsl;
    }
    std::printf("    %-20s %9u -> %-8u %9zu -> %-8zu\n", "all", totalBefore.instructions, totalAfter.instructions, totalBefore.glsl, totalAfter.glsl);
    HX_CHECK(measured >= 40);
    // Unrolling adds instructions on purpose (no loop control, constant indices); the code as a whole gets smaller.
    HX_CHECK(totalAfter.glsl < totalBefore.glsl);
}
