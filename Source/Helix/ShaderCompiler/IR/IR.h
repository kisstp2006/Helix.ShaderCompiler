#pragma once

#include <Helix/Core/Containers/Array.h>
#include <Helix/Core/Strings/String.h>

namespace helix::shadercompiler
{
    enum class BaseType : u8
    {
        Void,
        Bool,
        Int,
        UInt,
        Float,
        Struct,
        Texture2D,
        Sampler,
    };

    // Scalars are 1x1, vectors rows x 1, matrices rows x cols (square, column major).
    struct Type
    {
        BaseType base = BaseType::Void;
        u8 rows = 1;
        u8 cols = 1;
        u16 structIndex = 0;

        static constexpr Type Scalar(BaseType b) { return {b, 1, 1, 0}; }
        static constexpr Type Vector(BaseType b, u32 n) { return {b, u8(n), 1, 0}; }
        static constexpr Type Matrix(u32 n) { return {BaseType::Float, u8(n), u8(n), 0}; }
        static constexpr Type Struct(u32 index) { return {BaseType::Struct, 1, 1, u16(index)}; }

        constexpr bool operator==(const Type&) const = default;
        constexpr bool IsVoid() const { return base == BaseType::Void; }
        constexpr bool IsNumeric() const { return base == BaseType::Bool || base == BaseType::Int || base == BaseType::UInt || base == BaseType::Float; }
        constexpr bool IsScalar() const { return IsNumeric() && rows == 1 && cols == 1; }
        constexpr bool IsVector() const { return IsNumeric() && rows > 1 && cols == 1; }
        constexpr bool IsMatrix() const { return cols > 1; }
        constexpr bool IsInteger() const { return base == BaseType::Int || base == BaseType::UInt; }
        constexpr Type ScalarType() const { return Scalar(base); }
    };

    inline constexpr Type kVoid = Type::Scalar(BaseType::Void);
    inline constexpr Type kBool = Type::Scalar(BaseType::Bool);
    inline constexpr Type kInt = Type::Scalar(BaseType::Int);
    inline constexpr Type kUInt = Type::Scalar(BaseType::UInt);
    inline constexpr Type kFloat = Type::Scalar(BaseType::Float);

    // Role of a stage input or output.
    enum class Semantic : u8
    {
        None,
        Position,
        Location,
        Target,
        Depth,
        VertexId,
        InstanceId,
        FrontFacing,
        DispatchThreadId,
    };

    struct StructField
    {
        String name;
        Type type;
        Semantic semantic = Semantic::None;
        u32 semanticIndex = 0;
        bool flat = false;
    };

    struct StructType
    {
        String name;
        Array<StructField> fields;
    };

    // X(id, lower case name used by the shader library)
#define HX_SHADER_INTRINSICS(X)                                                                                              \
    X(Abs, "abs") X(Sign, "sign") X(Floor, "floor") X(Ceil, "ceil") X(Round, "round") X(Trunc, "trunc") X(Frac, "frac")        \
    X(Sqrt, "sqrt") X(Rsqrt, "rsqrt") X(Exp, "exp") X(Exp2, "exp2") X(Log, "log") X(Log2, "log2") X(Sin, "sin") X(Cos, "cos")  \
    X(Tan, "tan") X(Asin, "asin") X(Acos, "acos") X(Atan, "atan") X(Saturate, "saturate") X(Radians, "radians")                \
    X(Degrees, "degrees") X(Atan2, "atan2") X(Pow, "pow") X(Min, "min") X(Max, "max") X(Step, "step") X(Fmod, "fmod")          \
    X(Clamp, "clamp") X(Lerp, "lerp") X(Smoothstep, "smoothstep") X(Dot, "dot") X(Cross, "cross") X(Length, "length")          \
    X(Distance, "distance") X(Normalize, "normalize") X(Reflect, "reflect") X(Refract, "refract") X(Ddx, "ddx") X(Ddy, "ddy")  \
    X(Fwidth, "fwidth") X(Select, "select") X(Any, "any") X(All, "all") X(Mul, "mul") X(Transpose, "transpose")               \
    X(TextureSample, "texture.sample") X(TextureSampleLevel, "texture.samplelevel") X(TextureLoad, "texture.load")             \
    X(TextureSize, "texture.size")

    enum class Intrinsic : u8
    {
#define HX_INTRINSIC_ENUM(id, name) id,
        HX_SHADER_INTRINSICS(HX_INTRINSIC_ENUM)
#undef HX_INTRINSIC_ENUM
        Count,
    };

    const char* IntrinsicName(Intrinsic intrinsic);
    // Count when the name is unknown.
    Intrinsic FindIntrinsic(StringView lowerCaseName);

    enum class Op : u8
    {
        Neg,
        Not,
        BitNot,
        Add,
        Sub,
        Mul,
        Div,
        Rem,
        And,
        Or,
        Xor,
        Shl,
        Shr,
        LogicalAnd,
        LogicalOr,
        Eq,
        Ne,
        Lt,
        Le,
        Gt,
        Ge,
    };

    enum class ExprKind : u8
    {
        ConstFloat, // number
        ConstInt,   // integer, type Int or UInt
        ConstBool,  // integer 0 or 1
        Local,      // index into Function::locals
        Param,      // index into Function::params
        Global,     // index into Module::globals
        Field,      // a.field[index] of a struct
        Swizzle,    // a.swizzle
        Index,      // a[b]: vector component or matrix column
        Unary,      // op a
        Binary,     // a op b
        Cast,       // type(a)
        Construct,  // type(args); no arguments means the zero value
        Call,       // functions[index](args)
        Intrinsic,  // intrinsic(args)
    };

    using ExprId = u32;

    struct Expr
    {
        ExprKind kind = ExprKind::ConstInt;
        Op op = Op::Add;
        Intrinsic intrinsic = Intrinsic::Count;
        u8 swizzleCount = 0;
        u8 swizzle[4] = {};
        Type type;
        u32 index = 0;
        ExprId a = 0, b = 0;
        i64 integer = 0;
        f64 number = 0;
        Array<ExprId> args;
    };

    enum class StmtKind : u8
    {
        Assign,   // a = b
        Evaluate, // a (a call)
        Return,   // return a; a may be 0
        Discard,
        If,       // if (a) body else elseBody
        Loop,     // for (;;) body
        Break,
        Continue,
    };

    struct Stmt
    {
        StmtKind kind = StmtKind::Evaluate;
        ExprId a = 0, b = 0;
        Array<Stmt> body, elseBody;
    };

    enum class ParamMode : u8
    {
        In,
        Out,
        InOut,
    };

    struct Variable
    {
        String name;
        Type type;
        ParamMode mode = ParamMode::In;
        // Set on entry point parameters that are not structs.
        Semantic semantic = Semantic::None;
        u32 semanticIndex = 0;
        bool flat = false;
    };

    struct Function
    {
        String name;
        Type returnType;
        Array<Variable> params;
        Array<Variable> locals;
        // Expression arena; id 0 is the null expression.
        Array<Expr> exprs;
        Array<Stmt> body;
    };

    enum class GlobalKind : u8
    {
        Uniform,
        Texture,
        Sampler,
    };

    struct Global
    {
        String name;
        Type type;
        GlobalKind kind = GlobalKind::Uniform;
        u32 slot = 0;
    };

    enum class Stage : u8
    {
        Vertex,
        Fragment,
    };

    // One compiled entry point with everything it reaches.
    struct Module
    {
        Stage stage = Stage::Vertex;
        Array<StructType> structs;
        Array<Global> globals;
        // Callees come before their callers; the entry point is last.
        Array<Function> functions;
        u32 entry = 0;
        // Role of the entry point's return value when it is not a struct.
        Semantic returnSemantic = Semantic::None;
        u32 returnSemanticIndex = 0;
    };
}
