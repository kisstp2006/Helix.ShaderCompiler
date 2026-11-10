#pragma once

// Byte arrays of the assemblies built from Tests/Shaders and from Helix.Shaders, generated at build time.
#include <ShaderLibraryAssembly.h>
#include <TestShadersAssembly.h>

#include <Helix/Core/Basics/Span.h>

namespace helix::shadercompiler::testing
{
    inline Span<const u8> TestShadersImage() { return Span<const u8>(kTestShadersAssembly, sizeof(kTestShadersAssembly)); }
    inline Span<const u8> ShaderLibraryImage() { return Span<const u8>(kShaderLibraryAssembly, sizeof(kShaderLibraryAssembly)); }
}
