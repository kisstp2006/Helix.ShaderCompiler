#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    namespace
    {
        constexpr const char* kIntrinsicNames[] = {
#define HX_INTRINSIC_NAME(id, name) name,
            HX_SHADER_INTRINSICS(HX_INTRINSIC_NAME)
#undef HX_INTRINSIC_NAME
        };
    }

    const char* IntrinsicName(Intrinsic intrinsic) { return intrinsic < Intrinsic::Count ? kIntrinsicNames[usize(intrinsic)] : "?"; }

    namespace
    {
        void Scan(const Function& f, ExprId id, BindlessUse& use)
        {
            if (!id)
                return;
            const Expr& e = f.exprs[id];
            if (e.kind == ExprKind::Intrinsic)
            {
                use.textures = use.textures || e.intrinsic == Intrinsic::BindlessSample || e.intrinsic == Intrinsic::BindlessSampleLevel;
                use.buffers = use.buffers || e.intrinsic == Intrinsic::BindlessLoad || e.intrinsic == Intrinsic::BindlessStore ||
                              e.intrinsic == Intrinsic::BindlessSize;
                use.bufferWrites = use.bufferWrites || e.intrinsic == Intrinsic::BindlessStore;
            }
            Scan(f, e.a, use);
            Scan(f, e.b, use);
            for (ExprId arg : e.args)
                Scan(f, arg, use);
        }

        void Scan(const Function& f, const Array<Stmt>& stmts, BindlessUse& use)
        {
            for (const Stmt& s : stmts)
            {
                Scan(f, s.a, use);
                Scan(f, s.b, use);
                Scan(f, s.body, use);
                Scan(f, s.elseBody, use);
            }
        }
    }

    BindlessUse FindBindless(const Module& module)
    {
        BindlessUse use;
        for (const Function& f : module.functions)
            Scan(f, f.body, use);
        return use;
    }

    Intrinsic FindIntrinsic(StringView lowerCaseName)
    {
        for (u32 i = 0; i < u32(Intrinsic::Count); i++)
            if (lowerCaseName == kIntrinsicNames[i])
                return Intrinsic(i);
        return Intrinsic::Count;
    }
}
