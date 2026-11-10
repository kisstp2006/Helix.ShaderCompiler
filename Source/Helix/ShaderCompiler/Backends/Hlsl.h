#pragma once

#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    // The module as shader model 5 HLSL, for reading only: in error reports and dumps next to the bytecode the
    // Direct3D backend writes. It is not compiled anywhere (nothing in Helix needs an HLSL compiler).
    //
    // Matrices are written as the transpose of the C# ones: their rows are the C# columns, so m[i] stays column i,
    // float4x4(c0, c1, c2, c3) builds from columns and Mul(m, v) is mul(v, m); uniform blocks declare them row_major,
    // which matches the memory of the C# structs. Resources keep their slots as registers (b, t with s, u). Members the
    // RHI implements behind the bytecode (bindless tables) appear as hx_ helpers with a comment.
    bool EmitHlsl(const Module& module, String& out, String& error);
}
