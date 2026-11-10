#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/ShaderCompiler/Transforms/Passes.h>
#include <Helix/Testing/Test.h>

#include <cstdio>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

namespace
{
    bool Translate(const Assembly& a, StringView type, Module& module, const TranslateOptions& options = {})
    {
        String error;
        bool ok = TranslateEntryPoint(a, a.FindMethod(a.FindType("Helix.ShaderTests.Cases", type), "Fragment"), Stage::Fragment, module, error, options);
        if (!ok)
            std::printf("    %s\n", error.CStr());
        return ok;
    }

    // The first expression of a kind in the entry point; 0 when there is none.
    ExprId FindExpr(const Function& f, ExprKind kind)
    {
        for (ExprId id = 1; id < f.exprs.Size(); id++)
            if (f.exprs[id].kind == kind)
                return id;
        return 0;
    }

    void Expect(const Module& broken, StringView part)
    {
        String error;
        HX_CHECK(!VerifyModule(broken, error));
        std::printf("    %s\n", error.CStr());
        HX_CHECK(StringView(error).Contains(part));
    }
}

HX_TEST(Passes, RunInOrderAndCanBeDumped)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    struct Seen
    {
        Array<String> passes;
        String last;
    } seen;
    TranslateOptions options;
    options.passes.verify = true;
    options.passes.user = &seen;
    options.passes.dump = [](StringView pass, const Module& module, void* user) {
        Seen& s = *static_cast<Seen*>(user);
        s.passes.Push(String(pass));
        s.last.Clear();
        PrintModule(module, s.last);
    };
    Module module;
    HX_REQUIRE(Translate(a, "Lambdas", module, options));
    HX_REQUIRE(seen.passes.Size() == ShaderPasses().Size());
    for (usize i = 0; i < seen.passes.Size(); i++)
        HX_CHECK(seen.passes[i] == ShaderPasses()[i].name);
    HX_CHECK(seen.passes[0] == "simplify");
    HX_CHECK(StringView(seen.last).Contains(") entry") && StringView(seen.last).Contains("return "));
    String error;
    HX_CHECK(VerifyModule(module, error));
}

HX_TEST(Passes, VerifierFindsDamagedModules)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    Module good;
    HX_REQUIRE(Translate(a, "LocalFunctions", good));
    String error;
    HX_REQUIRE(VerifyModule(good, error));

    Module m = good;
    Function& entry = m.functions[m.entry];
    ExprId local = FindExpr(entry, ExprKind::Local);
    HX_REQUIRE(local != 0);
    entry.exprs[local].index = 1000;
    Expect(m, "local 1000 does not exist");

    m = good;
    m.functions[m.entry].body.Emplace().kind = StmtKind::Break;
    Expect(m, "outside a loop");

    m = good;
    ExprId call = FindExpr(m.functions[m.entry], ExprKind::Call);
    HX_REQUIRE(call != 0);
    m.functions[m.entry].exprs[call].args.Pop();
    Expect(m, "arguments");

    m = good;
    call = FindExpr(m.functions[m.entry], ExprKind::Call);
    m.functions[m.entry].exprs[call].index = m.entry;
    Expect(m, "recursion");

    m = good;
    {
        Function& f = m.functions[m.entry];
        Stmt& s = f.body.Emplace();
        s.kind = StmtKind::Assign;
        Expr target;
        target.kind = ExprKind::Local;
        target.index = 0;
        target.type = f.locals[0].type;
        Expr value;
        value.kind = ExprKind::ConstBool;
        value.type = kBool;
        s.a = AddExpr(f, Move(target));
        s.b = AddExpr(f, Move(value));
        if (f.locals[0].type == kBool)
            f.exprs[s.b].type = kFloat;
    }
    Expect(m, "another type");

    m = good;
    m.entry = u32(m.functions.Size());
    Expect(m, "entry point");
}

HX_TEST(Passes, RemoveDeadCodeAndCopies)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    // The module after the first pass and at the end.
    struct Seen
    {
        u32 calls = 0;
        usize localsFirst = 0, localsLast = 0, pickParams = 0;
    } seen;
    TranslateOptions options;
    options.passes.user = &seen;
    options.passes.dump = [](StringView, const Module& module, void* user) {
        Seen& s = *static_cast<Seen*>(user);
        usize locals = module.functions[module.entry].locals.Size();
        if (s.calls++ == 0)
            s.localsFirst = locals;
        s.localsLast = locals;
        for (const Function& f : module.functions)
            if (StringView(f.name).Contains("Pick"))
                s.pickParams = f.params.Size();
    };
    Module module;
    HX_REQUIRE(Translate(a, "DeadCode", module, options));
    std::printf("    entry locals: %zu after simplify, %zu at the end; Pick takes %zu\n", seen.localsFirst, seen.localsLast, seen.pickParams);
    // The C# compiler drops the unread local itself; the copy of the parameter is left to the passes.
    HX_CHECK(seen.localsFirst >= 1 && seen.localsLast == 0 && seen.pickParams == 1);
}
