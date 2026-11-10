#pragma once

// Byte arrays generated at build time: the assemblies built from Tests/Shaders, Tests/Includes and Helix.Shaders, and the
// oracle output.
#include <ShaderExpectations.h>
#include <ShaderIncludesAssembly.h>
#include <ShaderLibraryAssembly.h>
#include <TestShadersAssembly.h>
#include <TestShadersSymbols.h>

#include <Helix/Core/Basics/Span.h>

namespace helix::shadercompiler::testing
{
    inline Span<const u8> TestShadersImage() { return Span<const u8>(kTestShadersAssembly, sizeof(kTestShadersAssembly)); }
    // The portable PDB written next to the test shaders (the include assembly embeds its own).
    inline Span<const u8> TestShadersSymbols() { return Span<const u8>(kTestShadersSymbols, sizeof(kTestShadersSymbols)); }
    // Output of Tests/Oracle: what each case of Tests/Shaders/Cases produces on the CPU.
    inline Span<const u8> ShaderExpectations() { return Span<const u8>(kShaderExpectations, sizeof(kShaderExpectations)); }
    inline Span<const u8> ShaderLibraryImage() { return Span<const u8>(kShaderLibraryAssembly, sizeof(kShaderLibraryAssembly)); }
    // Shader code the test shaders call into, as an include.
    inline Span<const u8> ShaderIncludesImage() { return Span<const u8>(kShaderIncludesAssembly, sizeof(kShaderIncludesAssembly)); }
}
