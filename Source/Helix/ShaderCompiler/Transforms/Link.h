#pragma once

#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    // Fits the two stages of a variant to each other: vertex outputs at locations the fragment code never reads are not
    // written (fewer interpolators), and fragment inputs there are not declared (a vertex stage that leaves them out,
    // such as a shadow caster's, still links). Fails when the fragment stage reads a location the vertex stage does
    // not write. Call it after both modules are translated, before they go to a backend.
    bool LinkStages(Module& vertex, Module& fragment, String& error);
}
