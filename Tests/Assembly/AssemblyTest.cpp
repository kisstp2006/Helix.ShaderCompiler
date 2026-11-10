#include <Support/TestAssemblies.h>

#include <Helix/ShaderCompiler/Assembly/Assembly.h>
#include <Helix/ShaderCompiler/Assembly/IL.h>
#include <Helix/ShaderCompiler/Assembly/Signature.h>
#include <Helix/Testing/Test.h>

#include <cstdio>
#include <cstring>

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

namespace
{
    const char* kTests = "Helix.ShaderTests";

    // Attribute of a token by type name, with decoded arguments.
    struct Attribute
    {
        bool found = false;
        Array<AttributeArgument> fixed, named;

        const AttributeArgument* Named(StringView name) const
        {
            for (const AttributeArgument& a : named)
                if (a.name == name)
                    return &a;
            return nullptr;
        }
    };

    Attribute FindAttribute(const Assembly& assembly, Token parent, StringView name)
    {
        Attribute result;
        Array<CustomAttribute> attributes;
        assembly.Attributes(parent, attributes);
        for (const CustomAttribute& a : attributes)
            if (a.typeName == name && a.typeNamespace == "HelixEngine.Shaders")
                result.found = DecodeAttribute(assembly, a, result.fixed, result.named);
        return result;
    }

    // Opcodes of a method body in order.
    Array<Opcode> Opcodes(const Assembly& assembly, u32 method)
    {
        Array<Opcode> result;
        MethodBody body;
        if (!assembly.Body(method, body))
            return result;
        Instruction in;
        for (u32 at = 0; at < body.code.Size();)
        {
            if (!DecodeInstruction(body.code, at, in))
            {
                result.Clear();
                return result;
            }
            result.Push(in.opcode);
        }
        return result;
    }

    bool IsType(const Assembly& assembly, const TypeSig& sig, StringView name)
    {
        StringView ns, n;
        return (sig.kind == ElementType::ValueType || sig.kind == ElementType::GenericInst) && assembly.TypeName(sig.token, ns, n) && n == name &&
               ns == "HelixEngine.Shaders";
    }
}

HX_TEST(Assembly, LoadsTablesAndNames)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    HX_CHECK(a.Name() == "Helix.ShaderTests");
    HX_CHECK(a.RowCount(Table::TypeDef) >= 6u);
    u32 basics = a.FindType(kTests, "Basics");
    u32 lit = a.FindType(kTests, "Lit");
    HX_REQUIRE(basics != 0 && lit != 0);
    u32 forward = a.FindNestedType(lit, "Forward");
    HX_REQUIRE(forward != 0);
    HX_CHECK_EQ(a.TypeDef(forward).enclosingType, lit);
    HX_CHECK(a.FindNestedType(forward, "Draw") != 0 && a.FindNestedType(forward, "Varyings") != 0);
    HX_CHECK_EQ(a.FindType(kTests, "Forward"), 0u);

    StringView ns, name;
    HX_REQUIRE(a.TypeName(a.TypeDef(lit).extends, ns, name));
    HX_CHECK(name == "ShaderDefinition" && ns == "HelixEngine.Shaders");

    u32 add = a.FindMethod(basics, "Add");
    HX_REQUIRE(add != 0);
    HX_CHECK_EQ(a.DeclaringTypeOfMethod(add), basics);
    HX_CHECK(a.Method(add).IsStatic());
    MethodInfo outputs = a.Method(a.FindMethod(basics, "Outputs"));
    HX_REQUIRE(outputs.paramEnd - outputs.paramBegin == 3u);
    HX_CHECK(a.Param(outputs.paramBegin + 1).name == "doubled");
    HX_CHECK_EQ(a.Param(outputs.paramBegin + 2).sequence, 3u);
    u32 fog = a.FindField(lit, "Fog");
    HX_REQUIRE(fog != 0);
    HX_CHECK(a.Field(fog).IsStatic() && a.Field(fog).IsInitOnly());
    HX_CHECK_EQ(a.DeclaringTypeOfField(fog), lit);
}

HX_TEST(Assembly, DecodesSignatures)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    u32 basics = a.FindType(kTests, "Basics");
    MethodSig sig;
    HX_REQUIRE(ParseMethodSignature(a.Method(a.FindMethod(basics, "Add")).signature, sig));
    HX_CHECK(!sig.hasThis && sig.params.Size() == 2u && sig.returnType.kind == ElementType::R4 && sig.params[1].kind == ElementType::R4);

    HX_REQUIRE(ParseMethodSignature(a.Method(a.FindMethod(basics, "Scale")).signature, sig));
    HX_CHECK(IsType(a, sig.returnType, "float3") && IsType(a, sig.params[0], "float3") && sig.params[1].kind == ElementType::R4);

    HX_REQUIRE(ParseMethodSignature(a.Method(a.FindMethod(basics, "Outputs")).signature, sig));
    HX_REQUIRE(sig.params.Size() == 3u);
    HX_CHECK(sig.params[1].kind == ElementType::ByRef && sig.params[1].args[0].kind == ElementType::R4);
    HX_CHECK(sig.params[2].kind == ElementType::ByRef && IsType(a, sig.params[2].args[0], "float3"));

    HX_REQUIRE(ParseMethodSignature(a.Method(a.FindMethod(basics, "Identity")).signature, sig));
    HX_CHECK_EQ(sig.genericParamCount, 1u);
    HX_CHECK(sig.returnType.kind == ElementType::MVar && sig.params[0].kind == ElementType::MVar && sig.params[0].index == 0);

    u32 forward = a.FindNestedType(a.FindType(kTests, "Lit"), "Forward");
    TypeSig field;
    HX_REQUIRE(ParseFieldSignature(a.Field(a.FindField(forward, "counters")).signature, field));
    HX_CHECK(field.kind == ElementType::GenericInst && field.genericValueType && IsType(a, field, "RWStructuredBuffer`1"));
    HX_REQUIRE(field.args.Size() == 1u);
    HX_CHECK(field.args[0].kind == ElementType::U4);
    HX_REQUIRE(ParseFieldSignature(a.Field(a.FindField(forward, "albedo")).signature, field));
    HX_CHECK(IsType(a, field, "Texture2D"));

    MethodBody body;
    HX_REQUIRE(a.Body(a.FindMethod(basics, "Loop"), body));
    HX_REQUIRE(body.localsSignature.Is(Table::StandAloneSig));
    Array<TypeSig> locals;
    HX_REQUIRE(ParseLocalsSignature(a.StandAloneSignature(body.localsSignature.Row()), locals));
    HX_CHECK(locals.Size() == 2u && locals[0].kind == ElementType::I4 && locals[1].kind == ElementType::I4);
}

HX_TEST(Assembly, DecodesInstructions)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    u32 basics = a.FindType(kTests, "Basics");
    Array<Opcode> add = Opcodes(a, a.FindMethod(basics, "Add"));
    HX_REQUIRE(add.Size() == 4u);
    HX_CHECK(add[0] == Opcode::Ldarg0 && add[1] == Opcode::Ldarg1 && add[2] == Opcode::Add && add[3] == Opcode::Ret);
    HX_CHECK(std::strcmp(OpcodeName(Opcode::Ldarg0), "ldarg.0") == 0 && std::strcmp(OpcodeName(Opcode::Ceq), "ceq") == 0);

    // The loop has a backward conditional branch whose target lies inside the method.
    MethodBody body;
    HX_REQUIRE(a.Body(a.FindMethod(basics, "Loop"), body));
    bool backward = false, inside = true;
    Instruction in;
    for (u32 at = 0; at < body.code.Size();)
    {
        HX_REQUIRE(DecodeInstruction(body.code, at, in));
        if (in.operand == OperandKind::Branch8 || in.operand == OperandKind::Branch32)
        {
            backward = backward || in.target <= in.offset;
            inside = inside && in.target < body.code.Size();
        }
    }
    HX_CHECK(backward && inside);

    HX_REQUIRE(a.Body(a.FindMethod(basics, "Pick"), body));
    u32 switches = 0, floats = 0;
    for (u32 at = 0; at < body.code.Size();)
    {
        HX_REQUIRE(DecodeInstruction(body.code, at, in));
        if (in.opcode == Opcode::Switch)
        {
            switches++;
            HX_CHECK_EQ(in.switchCount, 4u);
            for (u32 i = 0; i < in.switchCount; i++)
                HX_CHECK(in.SwitchTarget(i) < body.code.Size());
        }
        if (in.opcode == Opcode::LdcR4)
            floats += in.number == 1.0 || in.number == 2.0 || in.number == 4.0 || in.number == 8.0 || in.number == 16.0 ? 1 : 0;
    }
    HX_CHECK_EQ(switches, 1u);
    HX_CHECK_EQ(floats, 5u);

    HX_REQUIRE(a.Body(a.FindMethod(basics, "Wide"), body));
    bool wide = false, half = false;
    for (u32 at = 0; at < body.code.Size();)
    {
        HX_REQUIRE(DecodeInstruction(body.code, at, in));
        wide = wide || (in.opcode == Opcode::LdcI8 && in.value == 1234567890123ll);
        half = half || (in.opcode == Opcode::LdcR8 && in.number == 0.5);
    }
    HX_CHECK(wide && half);

    // Scale calls the generic Identity through a MethodSpec and the float3 operator through a MemberRef.
    HX_REQUIRE(a.Body(a.FindMethod(basics, "Scale"), body));
    bool generic = false, multiply = false;
    for (u32 at = 0; at < body.code.Size();)
    {
        HX_REQUIRE(DecodeInstruction(body.code, at, in));
        if (in.opcode != Opcode::Call)
            continue;
        Token target(u32(in.value));
        if (target.Is(Table::MethodSpec))
        {
            Array<TypeSig> args;
            HX_REQUIRE(ParseMethodSpec(a.GetBlob(a.Cell(Table::MethodSpec, target.Row(), 1)), args));
            generic = args.Size() == 1 && IsType(a, args[0], "float3") && a.CodedCell(Table::MethodSpec, target.Row(), 0).Is(Table::MethodDef);
        }
        else if (target.Is(Table::MemberRef))
        {
            StringView ns, name;
            MemberRefInfo ref = a.MemberRef(target.Row());
            multiply = ref.name == "op_Multiply" && a.TypeName(ref.parent, ns, name) && name == "float3";
        }
    }
    HX_CHECK(generic && multiply);

    // Every method of the assembly decodes to its end.
    u32 methods = 0;
    for (u32 row = 1; row <= a.RowCount(Table::MethodDef); row++)
        if (a.Method(row).rva)
        {
            HX_CHECK(!Opcodes(a, row).Empty());
            methods++;
        }
    HX_CHECK(methods >= 10u);
}

HX_TEST(Assembly, DecodesShaderAttributes)
{
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));
    u32 lit = a.FindType(kTests, "Lit");
    u32 forward = a.FindNestedType(lit, "Forward");

    Attribute shader = FindAttribute(a, Token(Table::TypeDef, lit), "ShaderAttribute");
    HX_REQUIRE(shader.found && shader.fixed.Size() == 1u);
    HX_CHECK(shader.fixed[0].kind == AttributeArgument::Kind::Text && shader.fixed[0].text == "Tests/Lit");

    Attribute pass = FindAttribute(a, Token(Table::TypeDef, forward), "PassAttribute");
    HX_REQUIRE(pass.found && pass.fixed.Size() == 1u && pass.named.Size() == 3u);
    HX_CHECK(pass.fixed[0].text == "Forward");
    HX_REQUIRE(pass.Named("LightMode") && pass.Named("ZWrite") && pass.Named("Cull"));
    HX_CHECK(pass.Named("LightMode")->text == "ForwardBase");
    HX_CHECK(pass.Named("ZWrite")->kind == AttributeArgument::Kind::Boolean && pass.Named("ZWrite")->integer == 0);
    HX_CHECK_EQ(pass.Named("Cull")->integer, 1);

    Attribute range = FindAttribute(a, Token(Table::Field, a.FindField(lit, "smoothness")), "RangeAttribute");
    HX_REQUIRE(range.found && range.fixed.Size() == 2u);
    HX_CHECK(range.fixed[0].number == 0.0 && range.fixed[1].number == 1.0);
    Attribute keyword = FindAttribute(a, Token(Table::Field, a.FindField(lit, "Fog")), "KeywordAttribute");
    HX_REQUIRE(keyword.found && keyword.Named("ShaderFeature"));
    HX_CHECK(keyword.fixed[0].integer == 0 && keyword.Named("ShaderFeature")->integer == 1);

    Attribute uniform = FindAttribute(a, Token(Table::Field, a.FindField(forward, "draw")), "UniformAttribute");
    Attribute storage = FindAttribute(a, Token(Table::Field, a.FindField(forward, "counters")), "StorageAttribute");
    HX_REQUIRE(uniform.found && storage.found);
    HX_CHECK(uniform.fixed[0].integer == 2 && storage.fixed[0].integer == 3);

    u32 vertex = a.FindMethod(forward, "Vertex"), fragment = a.FindMethod(forward, "Fragment"), count = a.FindMethod(forward, "Count");
    HX_CHECK(FindAttribute(a, Token(Table::MethodDef, vertex), "VertexShaderAttribute").found);
    HX_CHECK(!FindAttribute(a, Token(Table::MethodDef, vertex), "FragmentShaderAttribute").found);
    Attribute compute = FindAttribute(a, Token(Table::MethodDef, count), "ComputeShaderAttribute");
    HX_REQUIRE(compute.found && compute.fixed.Size() == 3u);
    HX_CHECK(compute.fixed[0].integer == 8 && compute.fixed[1].integer == 4 && compute.fixed[2].integer == 1);

    // Parameter attributes: location of the second vertex input, target of the fragment return value.
    MethodInfo v = a.Method(vertex);
    bool uvLocation = false;
    for (u32 p = v.paramBegin; p < v.paramEnd; p++)
        if (a.Param(p).name == "uv")
        {
            Attribute location = FindAttribute(a, Token(Table::Param, p), "LocationAttribute");
            uvLocation = location.found && location.fixed[0].integer == 1;
        }
    HX_CHECK(uvLocation);
    MethodInfo f = a.Method(fragment);
    bool target = false;
    for (u32 p = f.paramBegin; p < f.paramEnd; p++)
        if (a.Param(p).sequence == 0)
            target = FindAttribute(a, Token(Table::Param, p), "TargetAttribute").found;
    HX_CHECK(target);

    // The library marks its functions with the operation they map to.
    Assembly library;
    HX_REQUIRE(library.Load(ShaderLibraryImage()));
    u32 math = library.FindType("HelixEngine.Shaders", "ShaderMath");
    HX_REQUIRE(math != 0);
    Attribute intrinsic = FindAttribute(library, Token(Table::MethodDef, library.FindMethod(math, "Cross")), "IntrinsicAttribute");
    HX_REQUIRE(intrinsic.found);
    HX_CHECK(intrinsic.fixed[0].text == "cross");
    Attribute vector = FindAttribute(library, Token(Table::TypeDef, library.FindType("HelixEngine.Shaders", "float3")), "IntrinsicTypeAttribute");
    HX_REQUIRE(vector.found);
    HX_CHECK(vector.fixed[0].text == "float3");
}

// Every byte of a damaged file may be wrong: loading and walking it must stay inside the image.
HX_TEST(Assembly, SurvivesDamagedFiles)
{
    Span<const u8> image = TestShadersImage();
    Assembly a;
    HX_CHECK(!a.Load(Span<const u8>()));
    HX_CHECK(!a.Load(Span<const u8>(image.Data(), 100)));
    HX_CHECK(!a.Load(Span<const u8>(image.Data(), image.Size() / 2)));
    HX_CHECK(!a.Error().Empty());

    Array<u8> damaged;
    damaged.Resize(image.Size());
    u32 state = 2024, loaded = 0;
    for (u32 round = 0; round < 400; round++)
    {
        std::memcpy(damaged.Data(), image.Data(), image.Size());
        u32 flips = 1 + round % 8;
        for (u32 i = 0; i < flips; i++)
        {
            state = state * 1664525u + 1013904223u;
            // Half of the damage goes to the headers and metadata at the start of the file.
            usize at = (i & 1) ? (state >> 8) % image.Size() : (state >> 8) % 4096;
            damaged[at] = u8(state >> 24);
        }
        if (!a.Load(damaged))
            continue;
        loaded++;
        Array<CustomAttribute> attributes;
        Array<AttributeArgument> fixed, named;
        for (u32 row = 1; row <= a.RowCount(Table::TypeDef); row++)
        {
            TypeDefInfo t = a.TypeDef(row);
            attributes.Clear();
            a.Attributes(Token(Table::TypeDef, row), attributes);
            for (const CustomAttribute& attribute : attributes)
                DecodeAttribute(a, attribute, fixed, named);
            for (u32 f = t.fieldBegin; f < t.fieldEnd; f++)
            {
                TypeSig sig;
                ParseFieldSignature(a.Field(f).signature, sig);
            }
            for (u32 m = t.methodBegin; m < t.methodEnd; m++)
            {
                MethodSig sig;
                ParseMethodSignature(a.Method(m).signature, sig);
                Opcodes(a, m);
            }
        }
    }
    std::printf("    %u of 400 damaged files still loaded\n", loaded);
}
