#pragma once

#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    struct DxbcShader
    {
        // A complete container (ISGN, OSGN, SHDR with the checksum) as Direct3D 11 takes it.
        Array<u8> bytes;
    };

    // Writes one module as Direct3D shader bytecode, shader model 4.0 (feature level 10.0 and up).
    //
    // Conventions of the output: a uniform struct with slot n is constant buffer b<n>, a texture with slot n is t<n> and
    // its sampler s<n>. Vertex inputs are TEXCOORD<location>; between the stages SV_Position is register 0 and
    // TEXCOORD<location> register location + 1; fragment targets are SV_Target<index>.
    // All calls are inlined, since the bytecode has no functions.
    bool EmitDxbc(const Module& module, DxbcShader& out, String& error);
}
