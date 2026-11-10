#include <Support/Fuzz.h>

#include <Helix/Testing/Test.h>

#include <cstdio>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

// A short run of the fuzzer of Tools/Fuzz: mutated assemblies and PDBs never crash a reader, the translation or a backend.
HX_TEST(Fuzz, MutatedAssembliesAreReadSafely)
{
    FuzzRound fuzz{};
    u32 translated = 0;
    for (u64 round = 0; round < 40; round++)
        translated += fuzz.Run(round);
    std::printf("    40 rounds, %u entry points translated\n", translated);
    HX_CHECK(translated > 0);
}
