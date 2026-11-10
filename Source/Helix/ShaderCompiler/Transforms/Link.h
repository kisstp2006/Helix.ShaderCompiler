#pragma once

#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    // Fits the vertex stage of a variant to its fragment stage: vertex outputs at locations the fragment code never
    // reads are not written (fewer interpolators). Fails when the fragment stage reads a location the vertex stage does
    // not write. Call it after both modules are translated, before the vertex module goes to a backend.
    bool LinkStages(Module& vertex, const Module& fragment, String& error);
}
