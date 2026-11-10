#pragma once

#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    // Evaluates what is constant and removes what cannot run: constant scalar expressions are folded, a conditional
    // with a constant condition is replaced by the branch taken, statements after a break, continue or return are
    // dropped, and functions and resources that are no longer referenced leave the module.
    // This is what turns the keywords of a variant into code without the branches of the other variants.
    void Simplify(Module& module);
}
