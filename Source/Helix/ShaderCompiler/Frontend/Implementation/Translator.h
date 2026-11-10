#pragma once

// The IL to IR translator, shared by the files of this folder: Types (type mapping and attributes), Expressions (the
// expression and stack helpers), Members (fields, resources, keywords), Delegates, Library (members of the shader
// library), Calls (functions, resolution across assemblies) and Blocks (IL instructions, blocks and methods).

#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/ShaderCompiler/Frontend/Implementation/Flow.h>

#include <Helix/Core/Strings/Format.h>
#include <Helix/ShaderCompiler/Assembly/IL.h>
#include <Helix/ShaderCompiler/Assembly/Signature.h>

namespace helix::shadercompiler::frontend
{
    inline constexpr StringView kLibrary = "HelixEngine.Shaders";

    // A value on the IL evaluation stack. Addresses (ldloca, ldflda, ref parameters) keep the expression of the
    // place they point to; reading through one yields the same expression.
    // Delegates exist only while compiling: a null object, a method pointer (ldftn) and a delegate made of the two
    // stand on the stack as entries of Method::delegates, never as IR values.
    enum class ItemKind : u8
    {
        Value,
        Null,
        Function,
        Delegate,
    };

    struct Item
    {
        ExprId e = 0;
        bool address = false;
        ItemKind kind = ItemKind::Value;
        i32 index = -1;
    };

    // Vector and matrix types of the shader library by name: float3, int2, bool4, float4x4, ...
    inline bool LibraryType(StringView name, Type& out)
    {
        static constexpr struct
        {
            StringView prefix;
            BaseType base;
        } kBases[] = {{"float", BaseType::Float}, {"int", BaseType::Int}, {"uint", BaseType::UInt}, {"bool", BaseType::Bool}};
        if (name.StartsWith("half"))
        {
            StringView rest = name.Substr(4);
            if (rest.Empty() || (rest.Size() == 1 && rest[0] >= '2' && rest[0] <= '4'))
            {
                out = Type::Half(rest.Empty() ? 1u : u32(rest[0] - '0'));
                return true;
            }
            return false;
        }
        // A bindless resource is the index it was registered under.
        if (name == "BindlessTexture" || name == "BindlessBuffer")
        {
            out = kUInt;
            return true;
        }
        if (name == "SamplerState" || name == "SamplerComparisonState")
        {
            out = Type::Scalar(BaseType::Sampler);
            return true;
        }
        for (const auto& b : kBases)
        {
            if (!name.StartsWith(b.prefix))
                continue;
            StringView rest = name.Substr(b.prefix.Size());
            if (rest.Size() == 1 && rest[0] >= '2' && rest[0] <= '4')
            {
                out = Type::Vector(b.base, u32(rest[0] - '0'));
                return true;
            }
            if (b.base == BaseType::Float && rest.Size() == 3 && rest[1] == 'x' && rest[0] == rest[2] && rest[0] >= '2' && rest[0] <= '4')
            {
                out = Type::Matrix(u32(rest[0] - '0'));
                return true;
            }
        }
        return false;
    }

    inline i32 ComponentIndex(char c) { return c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : c == 'w' ? 3 : -1; }

    struct Translator
    {
        // The assembly of the method being translated; assemblies[0] is the shader assembly, the others are references.
        const Assembly* a;
        Module& m;
        String& error;

        const TranslateOptions& options;
        Array<const Assembly*> assemblies;
        u32 current = 0;

        Translator(const Assembly& assembly, Module& module, String& e, const TranslateOptions& o) : a(&assembly), m(module), error(e), options(o)
        {
            assemblies.Push(&assembly);
            for (const Assembly* r : o.references)
                if (r && r != &assembly)
                    assemblies.Push(r);
            for (const Assembly* x : assemblies)
            {
                const DebugInfo* found = nullptr;
                for (const DebugInfo* d : o.symbols)
                    if (!found && d && d->Matches(*x))
                        found = d;
                symbols.Push(found);
            }
        }

        // Debug information per assembly, and the instruction being translated, for the file and line of errors.
        Array<const DebugInfo*> symbols;
        struct Location
        {
            u32 assembly = 0, method = 0, offset = 0;
        };
        Location location;

        // "file:line: " of an IL offset of a method of the current assembly; empty without debug information.
        String At(u32 assembly, u32 method, u32 offset) const;

        // Rows are per assembly; types are told apart by both.
        u64 Key(u32 row) const { return (u64(current) << 32) | row; }

        // Runs f with `assembly` as the current assembly.
        template <typename F>
        auto InAssembly(u32 assembly, F&& f)
        {
            const Assembly* saved = a;
            u32 savedIndex = current;
            a = assemblies[assembly];
            current = assembly;
            auto result = f();
            a = saved;
            current = savedIndex;
            return result;
        }

        // C# types already in the module: Key(row) of the TypeDef and the index in Module::structs or Module::arrays.
        struct Known
        {
            u64 key;
            u32 index;
        };
        Array<Known> knownStructs;
        Array<Known> knownArrays;

        static i32 FindKnown(const Array<Known>& known, u64 key)
        {
            for (const Known& k : known)
                if (k.key == key)
                    return i32(k.index);
            return -1;
        }
        struct FieldGlobal
        {
            u64 fieldRow;
            i32 global;
        };
        Array<FieldGlobal> fieldGlobals;
        // The block of dynamic keywords (-1 until a shader reads one) and the classes whose keywords it holds.
        i32 keywordGlobal = -1;
        Array<u64> keywordClasses;
        struct Compiled
        {
            String key;
            i32 index;
        };
        Array<Compiled> compiled;
        Array<String> building;

        // The method a delegate calls and the closure it carries (0 for none: a lambda without captures or a static
        // method).
        struct DelegateInfo
        {
            u32 assembly = 0;
            u32 target = 0;
            Array<Type> typeArgs;
            ExprId closure = 0;
        };
        // A delegate argument of a call: the callee is compiled for the method it calls, and the closure becomes an
        // inout parameter.
        struct Binding
        {
            u32 arg = 0;
            u32 assembly = 0;
            u32 target = 0;
            Array<Type> typeArgs;
            i32 closure = -1;
        };

        // ---- per method state ----
        struct Method
        {
            Function fn;
            StringView name;
            MethodSig sig;
            Array<Type> typeArgs;
            Array<u8> temporary; // per local: created by the translation, written once before use
            // Per IL local: 1 for a ref local (the C# compiler makes them for compound assignments), 2 for a Span over an
            // inline array, and the place it refers to.
            // 3 marks a delegate local, which stands for the delegate it was set to.
            Array<u8> isRef;
            Array<ExprId> refTarget;
            Array<i32> localDelegate;
            // Per IL argument: the parameter of the function (-1 when it has none: the unused this of a lambda without
            // captures, a delegate without closure) and the delegate a delegate argument stands for.
            Array<i32> argParam;
            Array<i32> argDelegate;
            Array<DelegateInfo> delegates;
            // Per parameter: something writes it. A ref parameter (also this) that nothing writes is passed by value,
            // so readonly members can be called on uniforms.
            Array<u8> written;
            Array<Instruction> code;
            Array<i32> blockOfOffset;
            Array<FlowBlock> blocks;
            struct BlockInfo
            {
                u32 first = 0, last = 0;
                Array<Item> entry;
                bool hasEntry = false;
                bool done = false;
            };
            Array<BlockInfo> info;
            Array<Item> stack;
            Array<Stmt>* stmts = nullptr;
        };

        bool Fail(StringView method, StringView message);

        // ---------------------------------------------------------------- types

        bool Has(Token parent, StringView attribute, Array<AttributeArgument>* fixed = nullptr) const;

        Semantic ReadSemantic(Token parent, u32& index, bool& flat) const;

        // Length of an [InlineArray] struct; 0 for other structs.
        u32 InlineArrayLength(u32 typeRow) const;

        // An [InlineArray] struct: its one instance field is the element.
        i32 ArrayFor(u32 typeRow, u32 length, StringView method);

        i32 StructFor(u32 typeRow, StringView method);

        // Enums are their integer value.
        bool IsEnum(u32 typeRow) const;

        bool MapType(const TypeSig& sig, const Method* method, Type& out, bool& byRef, StringView where);

        // ---------------------------------------------------------------- delegates

        // Variables captured by lambdas and local functions live in these.
        bool IsClosureType(u32 typeRow) const { return a->TypeDef(typeRow).name.StartsWith("<>c__DisplayClass"); }

        // The class of lambdas without captures and the one of cached method groups.
        bool IsLambdaHolder(u32 typeRow) const;

        // Func<>, Action<>, Action and delegate types declared in the shader assembly.
        bool IsDelegateTypeToken(Token t) const;

        bool IsDelegateType(const TypeSig& sig) const;

        bool MethodHasThis(u32 assembly, u32 row) const;

        static bool SameDelegate(const Method& mt, i32 x, i32 y);

        // newobj of a delegate type makes a delegate of the object and the method pointer; Invoke calls its method.
        bool DelegateCall(Method& mt, StringView member, Array<Item>& args, bool isNew);

        // An enum, inline array or struct defined in the current assembly.
        bool MapValueType(u32 row, Type& out, StringView where);

        // System.Span<T> or System.ReadOnlySpan<T>, which the C# compiler uses to index inline arrays.
        bool IsSpanType(const TypeSig& sig) const;

        // RWTexture2D<T> of the shader library.
        bool IsImageType(const TypeSig& sig) const;

        // Texture2D<T>, Texture2DArray<T>, Texture3D<T> or TextureCube<T> of the shader library; Void for other types.
        BaseType TextureType(const TypeSig& sig) const;

        // StructuredBuffer<T> or RWStructuredBuffer<T> of the shader library.
        bool IsBufferType(const TypeSig& sig, bool* writable = nullptr) const;

        // ---------------------------------------------------------------- expressions

        static Type TypeOf(const Method& mt, ExprId e) { return mt.fn.exprs[e].type; }

        static ExprId Leaf(Method& mt, ExprKind kind, u32 index, Type type);

        static ExprId ConstInt(Method& mt, i64 value, Type type);

        static ExprId ConstFloat(Method& mt, f64 value);

        static ExprId Cast(Method& mt, ExprId value, Type to);

        // IL has one integer type on the stack; the IR keeps int, uint and bool apart and converts at the uses.
        static ExprId Coerce(Method& mt, ExprId value, Type to);

        static u32 NewLocal(Method& mt, Type type, bool temporary);

        // True when the value depends on memory a later statement could change.
        static bool ReadsState(const Method& mt, ExprId id);

        static void Spill(Method& mt, Item& item);

        // Values still waiting on the stack must keep what they read before a statement changes memory.
        static void Flush(Method& mt);

        static void MarkWritten(Method& mt, ExprId place);

        static void Assign(Method& mt, ExprId target, ExprId value);

        static Item Pop(Method& mt);

        static void Push(Method& mt, ExprId e, bool address = false) { mt.stack.Push({e, address}); }

        static ExprId Swizzle(Method& mt, ExprId value, const u8* components, u32 count);

        static ExprId Index(Method& mt, ExprId value, ExprId index);

        // Brings the operands of an arithmetic or comparison instruction to one type.
        static void Unify(Method& mt, ExprId& x, ExprId& y, bool asUnsigned);

        static ExprId Arithmetic(Method& mt, Op op, ExprId x, ExprId y, bool asUnsigned);

        // `unordered` is the .un form: unsigned for integers, true for NaN operands for floats.
        static ExprId Compare(Method& mt, Op op, ExprId x, ExprId y, bool unordered);

        // ---------------------------------------------------------------- members

        // Field of a struct or component of a vector or matrix, for ldfld, ldflda and stfld.
        bool FieldExpr(Method& mt, Token token, ExprId object, ExprId& out);

        bool IsDynamicKeyword(u32 fieldRow) const;

        // The member of the keyword block for a dynamic keyword field. The block holds every dynamic keyword of the
        // class that declares it, in declaration order, so its layout is the same for every pass and variant.
        i32 KeywordMember(Method& mt, u32 fieldRow);

        // Static fields are resources (uniform block, texture, sampler) or keywords.
        bool StaticField(Method& mt, Token token, Item& out);

        // ---------------------------------------------------------------- calls

        // Members of Texture2D<T>: results have the texel type of the texture. The sampler travels with the texture slot,
        // so only coordinates and level remain of the arguments.
        // The global a resource member is called on: the static field itself, of the kind `accepts` takes; -1 after an
        // error naming `what`.
        template <typename F>
        i32 ResourceOf(Method& mt, ExprId self, StringView what, F&& accepts)
        {
            const Expr& e = mt.fn.exprs[self];
            if (e.kind != ExprKind::Global || !accepts(m.globals[e.index]))
                return Fail(mt.name, Format("a {} must be used directly from its static field", what)), -1;
            return i32(e.index);
        }
        static ExprId MakeIntrinsic(Method& mt, Intrinsic which, Type type, Span<const ExprId> args);
        bool TextureCall(Method& mt, StringView member, Array<Item>& args);

        // The inline array helpers of the C# compiler and the Span members used with them; a span or an element ref is the
        // place of the array or of the element.
        bool ArrayCall(Method& mt, StringView member, Array<Item>& args);

        // Members of RWTexture2D<T>: a texel is the Index expression of the image with an int2.
        bool ImageCall(Method& mt, StringView member, Array<Item>& args);

        // Indexer and length of a storage buffer; an element is the Index expression of the buffer.
        bool BufferCall(Method& mt, StringView member, Array<Item>& args);

        // Members of the vector and matrix types and the functions of the shader library.
        // `unknown` is set (without an error) for members the compiler has no intrinsic for; their C# body may compile.
        bool LibraryCall(Method& mt, StringView typeName, StringView member, const MethodSig& sig, Array<Item>& args, bool isNew, bool& unknown);

        i32 FunctionFor(u32 methodRow, const Array<Type>& typeArgs, StringView caller, const Array<Binding>& bindings = Array<Binding>());

        // FunctionFor for a method of another assembly; the translation reads that assembly.
        i32 FunctionIn(u32 assembly, u32 methodRow, const Array<Type>& typeArgs, StringView caller, const Array<Binding>& bindings);

        // The TypeDef a TypeDef or TypeRef token of the current assembly stands for, in whichever assembly defines it.
        bool ResolveType(Token type, u32& assembly, u32& typeRow) const;

        static bool SameType(const Assembly& x, const TypeSig& s, const Assembly& y, const TypeSig& t);

        // The MethodDef a MemberRef of the current assembly names: same type, name and signature.
        bool ResolveMethodRef(Token token, u32& assembly, u32& row) const;

        // A method of the shader library the compiler has no intrinsic for: its C# body is compiled like user code.
        bool LibraryBody(Method& mt, Token method, const Array<Type>& typeArgs, StringView typeName, StringView member, Array<Item>& args, bool isNew);

        bool Call(Method& mt, Token token, bool isNew);

        // A call of a method of the shader assembly. Delegate arguments select the compiled form of the callee and pass
        // their closure; a null `this` (lambdas without captures) passes nothing.
        bool UserCall(Method& mt, u32 assembly, u32 row, const Array<Type>& typeArgs, Array<Item>& args);

        // ---------------------------------------------------------------- blocks

        // Values left on the stack at the end of a block travel to the next blocks in temporaries.
        void LeaveBlock(Method& mt, i32 target);

        bool TranslateBlock(Method& mt, u32 blockIndex);

        bool TranslateMethod(u32 row, const Array<Type>& typeArgs, const Array<Binding>& bindings, Function& out);
    };
}
