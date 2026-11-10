#pragma once

#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    struct DxbcShader
    {
        // A complete container (ISGN, OSGN, SHDR with the checksum) as Direct3D 11 takes it.
        Array<u8> bytes;
    };

    // Writes one module as Direct3D shader bytecode, shader model 4.0 (feature level 10.0 and up) where that is enough.
    //
    // Conventions of the output: a uniform struct with slot n is constant buffer b<n>, a texture with slot n is t<n> and
    // its sampler s<n>. Vertex inputs are TEXCOORD<location>; between the stages SV_Position is register 0 and
    // TEXCOORD<location> register location + 1; fragment targets are SV_Target<index>.
    // All calls are inlined, since the bytecode has no functions.
    //
    // Compute shaders, and shaders that use storage resources, are shader model 5.0 (feature level 11.0). A writable
    // buffer in slot n is u<n>, a read only one t<16 + n>, both as raw (byte address) views, so any buffer can be bound,
    // also one that is a vertex buffer or holds indirect arguments; elements are read packed as C# lays them out.
    // A storage texture in slot n is the typed view u<n>. [DispatchThreadId] is SV_DispatchThreadID.
    bool EmitDxbc(const Module& module, DxbcShader& out, String& error);
}
