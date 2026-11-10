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

    Intrinsic FindIntrinsic(StringView lowerCaseName)
    {
        for (u32 i = 0; i < u32(Intrinsic::Count); i++)
            if (lowerCaseName == kIntrinsicNames[i])
                return Intrinsic(i);
        return Intrinsic::Count;
    }
}
