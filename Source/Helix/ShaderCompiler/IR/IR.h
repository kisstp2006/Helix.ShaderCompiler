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
        Texture2DArray,
        Texture3D,
        TextureCube,
        Sampler,
        // A storage buffer: an array of elements, indexed with the Index expression. The element type is in its Global.
        Buffer,
        // A storage texture a shader writes: a texel is the Index expression of the texture with an int2.
        Image,
        // A fixed size array ([InlineArray]); structIndex is its index in Module::arrays, an element is an Index expression.
        Array,
    };

    // Scalars are 1x1, vectors rows x 1, matrices rows x cols (square, column major).
    struct Type
    {
        BaseType base = BaseType::Void;
        u8 rows = 1;
        u8 cols = 1;
        u16 structIndex = 0;
        // Float values that need only 16 bits of precision (half): computed as float, lower precision where it exists.
        u8 half = 0;

        static constexpr Type Scalar(BaseType b) { return {b, 1, 1, 0}; }
        static constexpr Type Vector(BaseType b, u32 n) { return {b, u8(n), 1, 0}; }
        static constexpr Type Matrix(u32 n) { return {BaseType::Float, u8(n), u8(n), 0}; }
        static constexpr Type Struct(u32 index) { return {BaseType::Struct, 1, 1, u16(index)}; }
        static constexpr Type ArrayOf(u32 index) { return {BaseType::Array, 1, 1, u16(index)}; }

        constexpr bool operator==(const Type&) const = default;
        constexpr bool IsVoid() const { return base == BaseType::Void; }
        constexpr bool IsNumeric() const { return base == BaseType::Bool || base == BaseType::Int || base == BaseType::UInt || base == BaseType::Float; }
        constexpr bool IsScalar() const { return IsNumeric() && rows == 1 && cols == 1; }
        constexpr bool IsVector() const { return IsNumeric() && rows > 1 && cols == 1; }
        constexpr bool IsMatrix() const { return cols > 1; }
        constexpr bool IsInteger() const { return base == BaseType::Int || base == BaseType::UInt; }
        constexpr Type ScalarType() const { return {base, 1, 1, 0, half}; }
        static constexpr Type Half(u32 n) { return {BaseType::Float, u8(n), 1, 0, 1}; }
        constexpr bool IsTexture() const
        {
            return base == BaseType::Texture2D || base == BaseType::Texture2DArray || base == BaseType::Texture3D || base == BaseType::TextureCube;
        }
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

    struct ArrayType
    {
        Type element;
        u32 length = 0;
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
    X(TextureSize, "texture.size") X(TextureSampleCmp, "texture.samplecmp")                         \
    X(TextureSampleCmpLevelZero, "texture.samplecmplevelzero") X(BufferLength, "buffer.length") X(InterlockedAdd, "interlockedadd") X(ImageSize, "image.size")                         \
    X(BindlessSample, "bindless.sample") X(BindlessSampleLevel, "bindless.samplelevel") X(BindlessLoad, "bindless.load")       \
    X(BindlessStore, "bindless.store") X(BindlessSize, "bindless.size") X(AsFloat, "asfloat") X(AsUInt, "asuint")            \
    X(AsInt, "asint")

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

    // Bindless textures and buffers are uint indices in the IR; these intrinsics reach the resources behind them.
    struct BindlessUse
    {
        bool textures = false, buffers = false, bufferWrites = false;
    };
    struct Module;
    // What the statements of the module (not dead expressions) use.
    BindlessUse FindBindless(const Module& module);

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

    // Values of the C# enum of the shader library.
    enum class StorageFormat : u8
    {
        Auto,
        RGBA8,
        RGBA16F,
        RGBA32F,
        R32F,
        R32UI,
        R32I,
    };

    enum class GlobalKind : u8
    {
        Uniform,
        Texture,
        Sampler,
        Storage,
    };

    struct Global
    {
        String name;
        Type type;
        GlobalKind kind = GlobalKind::Uniform;
        u32 slot = 0;
        // Storage buffers: the type of one element and whether the shader may write it.
        Type element;
        bool writable = false;
        // Storage textures: the texel format.
        StorageFormat format = StorageFormat::Auto;
        // Textures: sampled with a comparison sampler (shadow maps).
        bool comparison = false;
        // Storage textures: the shader reads texels too.
        bool readable = false;
    };

    enum class Stage : u8
    {
        Vertex,
        Fragment,
        Compute,
    };

    // One compiled entry point with everything it reaches.
    // A keyword the translated code read, with the value this variant was compiled for.
    struct KeywordUse
    {
        String name;
        bool isEnum = false;
        // A dynamic keyword is read from the keyword block at run time; value is then 0.
        bool dynamic = false;
        i64 value = 0;
    };

    // Dynamic keywords live in a uniform block the compiler adds: one uint per keyword, at the last uniform slot of the
    // RHI (kMaxUniformSlots = 12).
    inline constexpr u32 kKeywordBlockSlot = 11;
    inline constexpr const char* kKeywordBlockName = "hx_keywords";

    struct Module
    {
        Stage stage = Stage::Vertex;
        Array<StructType> structs;
        // Arrays come before the structs and arrays that contain them.
        Array<ArrayType> arrays;
        Array<Global> globals;
        // Callees come before their callers; the entry point is last.
        Array<Function> functions;
        u32 entry = 0;
        // Role of the entry point's return value when it is not a struct.
        Semantic returnSemantic = Semantic::None;
        u32 returnSemanticIndex = 0;
        // Compute: invocations per work group in x, y and z.
        u32 groupSize[3] = {1, 1, 1};
        // Keywords that decide this code; a variant differs from another only when one of these differs.
        Array<KeywordUse> keywords;
    };
}
