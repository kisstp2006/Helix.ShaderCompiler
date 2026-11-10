#pragma once

#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    // Checks that a module is well formed: every expression, local, parameter, global, struct, array and function a
    // module refers to exists; fields, swizzles and indices fit the types they apply to; assignments write places with
    // values of their type; calls pass one argument per parameter and only call earlier functions (no recursion);
    // conditions are bool; break and continue are inside loops; returns fit the return type. Fails naming the function
    // and the problem.
    bool VerifyModule(const Module& module, String& error);

    // The module as text, one statement per line: for dumps and for reading what a pass did.
    void PrintModule(const Module& module, String& out);

    // A pass changes a module in place; it fails with a message when the module cannot be handled.
    struct Pass
    {
        StringView name;
        bool (*run)(Module& module, String& error);
        // Needed for correct code (simplify removes the branches of other variants); the others only optimize.
        bool required = false;
    };

    // The passes in the order they run. A new pass is one entry of this table.
    Span<const Pass> ShaderPasses();

    struct PassOptions
    {
        // Verify after every pass (and before the first): on in debug builds, and in the tests.
#ifdef NDEBUG
        bool verify = false;
#else
        bool verify = true;
#endif
        // Off: only the required passes run, as a reference for what the others gain.
        bool optimize = true;
        // Called with the module after every pass, for debugging.
        void (*dump)(StringView pass, const Module& module, void* user) = nullptr;
        void* user = nullptr;
    };

    // Runs every pass of ShaderPasses on the module.
    bool RunPasses(Module& module, String& error, const PassOptions& options = {});
}
