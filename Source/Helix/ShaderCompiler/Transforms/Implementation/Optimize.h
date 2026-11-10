#pragma once

#include <Helix/ShaderCompiler/IR/IR.h>

// The optimization passes of the pass table, one file each.
namespace helix::shadercompiler::passes
{
    // A local that is written once, as a whole, with a parameter nothing writes, a uniform, or another such local, is
    // that value wherever it is read; its assignment goes away.
    bool PropagateCopies(Module& module, String& error);

    // Counted loops whose trip count is known become one copy of the body per trip: always with [Unroll] (up to 256
    // trips), never with [Loop], else when short (8 trips) and small (64 statements in all).
    bool UnrollLoops(Module& module, String& error);

    // A struct local that is never used as a whole (only its fields, and the zero value assigned) becomes one local per
    // field; nested structs come apart one level after the other.
    bool ReplaceStructLocals(Module& module, String& error);

    // An operation computed again in a straight run of statements, with the variables it reads unchanged, reads the
    // local the first statement now computes it into.
    bool EliminateCommonSubexpressions(Module& module, String& error);

    // Assignments to locals nothing reads (when the value has no effect), then locals nothing refers to, then
    // parameters of helper functions nothing reads (with their arguments, when those have no effect).
    bool RemoveDeadCode(Module& module, String& error);

    // Shared by the passes.
    bool IsConstant(const Expr& e);
    // Whether evaluating the expression changes anything: a call of a function with effects or with out parameters, an
    // atomic or a bindless store. `pure` holds per function whether a call of it has no effect.
    bool HasEffects(const Module& module, const Function& function, ExprId id, const Array<u8>& pure);
    // Per function: it writes no global, no out or inout parameter of its own, discards nothing and calls only such
    // functions.
    Array<u8> PureFunctions(const Module& module);
    // The local a place is rooted at (-1 for none); `whole` is false for a field, component or element of it.
    i32 RootLocal(const Function& function, ExprId place, bool& whole);
    // Calls f for every statement of a body, nested ones included.
    template <typename F>
    void ForEachStatement(const Array<Stmt>& stmts, F&& f)
    {
        for (const Stmt& s : stmts)
        {
            f(s);
            ForEachStatement(s.body, f);
            ForEachStatement(s.elseBody, f);
        }
    }
}
