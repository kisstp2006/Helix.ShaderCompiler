#pragma once

#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Assembly/DebugInfo.h>
#include <Helix/ShaderCompiler/Backends/Dxbc.h>
#include <Helix/ShaderCompiler/Backends/Glsl.h>
#include <Helix/ShaderCompiler/Backends/Hlsl.h>
#include <Helix/ShaderCompiler/Definition/ShaderDefinition.h>
#include <Helix/ShaderCompiler/Frontend/Frontend.h>

#include <cstdio>
#include <cstdlib>

namespace helix::shadercompiler::testing
{
    // One round of fuzzing, the same for a seed every time: a copy of one of the test images (the shader assembly, the
    // library, the include assembly with its embedded PDB, or the PDB of the shaders) with a few bytes changed, then
    // everything that reads it: the assembly reader, the PDB reader, the definition readers, the translation of every
    // entry point and the three backends. It returns how many entry points translated; it must never crash or hang.
    struct FuzzRound
    {
        u64 state;

        u32 Next()
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return u32(state >> 16);
        }

        void Mutate(Array<u8>& bytes)
        {
            u32 edits = 1 + Next() % 16;
            for (u32 e = 0; e < edits && !bytes.Empty(); e++)
            {
                usize at = Next() % bytes.Size();
                switch (Next() % 6)
                {
                case 0: bytes[at] ^= u8(1u << (Next() % 8)); break;
                case 1: bytes[at] = u8(Next()); break;
                case 2:
                {
                    static constexpr u8 kInteresting[] = {0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF};
                    bytes[at] = kInteresting[Next() % 6];
                    break;
                }
                case 3:
                {
                    // A run of bytes copied from elsewhere in the file.
                    usize from = Next() % bytes.Size(), count = 1 + Next() % 32;
                    for (usize i = 0; i < count && at + i < bytes.Size() && from + i < bytes.Size(); i++)
                        bytes[at + i] = bytes[from + i];
                    break;
                }
                case 4:
                    // A 32 bit value, as table indices and offsets are.
                    for (usize i = 0; i < 4 && at + i < bytes.Size(); i++)
                        bytes[at + i] = u8(Next());
                    break;
                default:
                    if (Next() % 8 == 0)
                        bytes.Resize(at + 1);
                    break;
                }
            }
        }

        u32 Run(u64 seed)
        {
            state = seed * 0x9E3779B97F4A7C15ull + 1;
            Span<const u8> images[] = {TestShadersImage(), ShaderLibraryImage(), ShaderIncludesImage(), TestShadersSymbols()};
            u32 which = Next() % 4;
            Array<u8> bytes;
            bytes.Append(images[which].Data(), images[which].Size());
            Mutate(bytes);
            Span<const u8> mutated(bytes.Data(), bytes.Size());

            // HELIX_FUZZ_TRACE names every step, to find the one that fails in a round.
            bool trace = std::getenv("HELIX_FUZZ_TRACE") != nullptr;
            auto step = [&](const char* what, StringView name = {}) {
                if (trace)
                {
                    std::printf("  %s %s\n", what, String(name).CStr());
                    std::fflush(stdout);
                }
            };
            step(which == 0 ? "load the shaders" : which == 1 ? "load the library" : which == 2 ? "load the includes" : "load the pdb");
            Assembly tests, library, includes;
            bool ok = (which == 0 ? tests.Load(mutated) : tests.Load(TestShadersImage())) &&
                      (which == 1 ? library.Load(mutated) : library.Load(ShaderLibraryImage())) &&
                      (which == 2 ? includes.Load(mutated) : includes.Load(ShaderIncludesImage()));
            DebugInfo symbols, embedded;
            bool sideBySide = which == 3 ? symbols.Load(mutated) : symbols.Load(TestShadersSymbols());
            embedded.LoadEmbedded(includes);
            if (!ok)
                return 0;
            const Assembly* references[] = {&library, &includes};
            const DebugInfo* debug[] = {&symbols, &embedded};
            TranslateOptions options;
            options.references = Span<const Assembly* const>(references, 2);
            options.symbols = Span<const DebugInfo* const>(debug, sideBySide ? 2 : 1);
            options.passes.verify = true;

            step("definitions");
            Array<ShaderDescription> shaders;
            Array<ComputeDescription> computes;
            String error;
            ReadShaderDefinitions(tests, shaders, error, options.references);
            ReadComputeDefinitions(tests, computes, error);

            u32 translated = 0;
            const Assembly& a = tests;
            for (u32 method = 1; method <= a.RowCount(Table::MethodDef); method++)
            {
                Array<CustomAttribute> attributes;
                a.Attributes(Token(Table::MethodDef, method), attributes);
                Stage stage = Stage::Vertex;
                bool entry = false;
                for (const CustomAttribute& c : attributes)
                {
                    entry = entry || c.typeName == "VertexShaderAttribute" || c.typeName == "FragmentShaderAttribute" || c.typeName == "ComputeShaderAttribute";
                    if (c.typeName == "FragmentShaderAttribute")
                        stage = Stage::Fragment;
                    if (c.typeName == "ComputeShaderAttribute")
                        stage = Stage::Compute;
                }
                if (!entry)
                    continue;
                step("translate", a.Method(method).name);
                String file;
                u32 line = 0;
                symbols.Find(method, Next() % 64, file, line);
                Module module;
                if (!TranslateEntryPoint(a, method, stage, module, error, options))
                    continue;
                translated++;
                GlslShader glsl;
                DxbcShader dxbc;
                String hlsl;
                step("glsl");
                EmitGlsl(module, stage == Stage::Compute ? GlslTarget::Glsl430 : GlslTarget::Glsl330, glsl, error);
                step("dxbc");
                EmitDxbc(module, dxbc, error);
                step("hlsl");
                EmitHlsl(module, hlsl, error);
            }
            return translated;
        }
    };
}
