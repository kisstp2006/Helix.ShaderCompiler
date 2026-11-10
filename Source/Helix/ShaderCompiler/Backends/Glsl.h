#pragma once

#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    enum class GlslTarget : u8
    {
        Glsl330, // desktop OpenGL 3.3 core
        Glsl450, // desktop OpenGL 4.5 core
        Essl300, // OpenGL ES 3.0 and WebGL 2
    };

    // A name in the generated source and the slot the shader author gave it; the renderer binds by these.
    struct GlslBinding
    {
        String name;
        GlobalKind kind = GlobalKind::Uniform;
        u32 slot = 0;
    };

    struct GlslShader
    {
        String source;
        Array<GlslBinding> bindings;
    };

    // Writes one module as a complete GLSL shader.
    //
    // Conventions of the output: vertex inputs are `hx_in<location>`, values passed from the vertex to the fragment stage
    // `hx_v<location>`, fragment outputs `hx_target<index>`; a uniform struct `name` lives in the std140 block
    // `hx_block_name`, a texture `name` is the sampler `g_name`.
    // Uniform structs must have the same layout in C# and in std140; the error names the field that needs padding.
    bool EmitGlsl(const Module& module, GlslTarget target, GlslShader& out, String& error);
}
