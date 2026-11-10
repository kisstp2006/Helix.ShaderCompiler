#pragma once

#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    // A basic block of the control flow graph the IL translation produces.
    struct FlowBlock
    {
        enum class End : u8
        {
            Goto,   // succ[0]
            Branch, // value ? succ[0] : succ[1]
            Return, // value may be 0
        };
        Array<Stmt> stmts;
        End end = End::Return;
        ExprId value = 0;
        i32 succ[2] = {-1, -1};
    };

    // Turns the graph (entry is block 0) into structured statements: loops, conditionals, break and continue.
    // Short circuit conditions are merged, return blocks are duplicated where several paths end in them.
    // Fails with a message for control flow that has no structured form (goto into shared code, irreducible loops).
    bool StructureFlow(Array<FlowBlock>& blocks, Function& function, Array<Stmt>& out, String& error);

    // Helpers shared by the translation steps.
    ExprId MakeNot(Function& function, ExprId condition);
}
