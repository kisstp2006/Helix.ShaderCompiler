#include <Helix/ShaderCompiler/Backends/Hlsl.h>

#include <Helix/Core/Strings/Format.h>

#include <cmath>
#include <cstdio>

namespace helix::shadercompiler
{
    namespace
    {
        String Ident(StringView name)
        {
            String out;
            for (char c : name)
            {
                bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                if (valid)
                    out.Append(c);
                else if (!out.Empty() && out[out.Size() - 1] != '_')
                    out.Append('_');
            }
            while (!out.Empty() && out[out.Size() - 1] == '_')
                out.Resize(out.Size() - 1);
            return out;
        }

        struct Writer
        {
            const Module& m;
            String& out;
            String& error;
            const Function* fn = nullptr;
            u32 indent = 0;
            // Helpers the code uses, written before the functions.
            Array<String> helpers;

            Writer(const Module& module, String& text, String& e) : m(module), out(text), error(e) {}

            bool Fail(StringView message)
            {
                if (error.Empty())
                    error = message;
                return false;
            }

            void Helper(String text)
            {
                for (const String& h : helpers)
                    if (h == text)
                        return;
                helpers.Push(Move(text));
            }

            // A struct's name from its qualified C# name, so a uniform block's type reads the same in both stages; the
            // index is added only when two structs of the module have the same name.
            String StructName(u32 index) const
            {
                const StructType& s = m.structs[index];
                StringView name = s.qualifiedName.Empty() ? StringView(s.name) : StringView(s.qualifiedName);
                for (u32 i = 0; i < m.structs.Size(); i++)
                {
                    const StructType& o = m.structs[i];
                    if (i != index && (o.qualifiedName.Empty() ? StringView(o.name) : StringView(o.qualifiedName)) == name)
                        return Format("S_{}_{}", Ident(name), index);
                }
                return Format("S_{}", Ident(name));
            }

            String TypeName(Type t) const
            {
                static constexpr const char* kScalar[] = {"void", "bool", "int", "uint", "float"};
                if (t.base == BaseType::Struct)
                    return StructName(t.structIndex);
                if (t.base == BaseType::Array)
                    return TypeName(m.arrays[t.structIndex].element);
                if (u32(t.base) > u32(BaseType::Float))
                    return "uint";
                const char* scalar = t.half && t.base == BaseType::Float ? "min16float" : kScalar[u32(t.base)];
                if (t.IsMatrix())
                    return Format("{}{}x{}", scalar, u32(t.cols), u32(t.rows));
                if (t.rows > 1)
                    return Format("{}{}", scalar, u32(t.rows));
                return scalar;
            }

            // Arrays are declared on the name.
            String Declare(Type t, StringView name) const
            {
                if (t.base == BaseType::Array)
                    return Format("{} {}[{}]", TypeName(t), name, m.arrays[t.structIndex].length);
                return Format("{} {}", TypeName(t), name);
            }

            static const char* SemanticName(Semantic s, Stage stage, bool input)
            {
                switch (s)
                {
                case Semantic::Position: return input && stage == Stage::Fragment ? "SV_Position" : input ? nullptr : "SV_Position";
                case Semantic::Location: return "TEXCOORD";
                case Semantic::Target: return "SV_Target";
                case Semantic::Depth: return "SV_Depth";
                case Semantic::VertexId: return "SV_VertexID";
                case Semantic::InstanceId: return "SV_InstanceID";
                case Semantic::FrontFacing: return "SV_IsFrontFace";
                case Semantic::DispatchThreadId: return "SV_DispatchThreadID";
                default: return nullptr;
                }
            }

            void WriteSemantic(Semantic s, u32 index, bool input)
            {
                const char* name = SemanticName(s, m.stage, input);
                if (!name)
                    return;
                if (s == Semantic::Location || s == Semantic::Target)
                    FormatAppend(out, " : {}{}", name, index);
                else
                    FormatAppend(out, " : {}", name);
            }

            void Number(f64 v)
            {
                if (std::isinf(v))
                {
                    out.Append(v > 0 ? "(1.0 / 0.0)" : "(-1.0 / 0.0)");
                    return;
                }
                char text[48];
                std::snprintf(text, sizeof(text), "%.9g", v);
                out.Append(text);
                bool point = false;
                for (const char* c = text; *c; c++)
                    point = point || *c == '.' || *c == 'e' || *c == 'n' || *c == 'i';
                if (!point)
                    out.Append(".0");
            }

            void Args(const Expr& e, usize from = 0)
            {
                for (usize i = from; i < e.args.Size(); i++)
                {
                    if (i > from)
                        out.Append(", ");
                    Value(e.args[i]);
                }
            }

            void Call(const char* name, const Expr& e, usize from = 0)
            {
                out.Append(name);
                out.Append('(');
                Args(e, from);
                out.Append(')');
            }

            void WriteIntrinsic(const Expr& e)
            {
                switch (e.intrinsic)
                {
                case Intrinsic::Select:
                    out.Append('(');
                    Value(e.args[0]);
                    out.Append(" ? ");
                    Value(e.args[1]);
                    out.Append(" : ");
                    Value(e.args[2]);
                    out.Append(')');
                    return;
                case Intrinsic::Mul:
                {
                    // The HLSL matrices are the transposes of the C# ones.
                    out.Append("mul(");
                    Value(e.args[1]);
                    out.Append(", ");
                    Value(e.args[0]);
                    out.Append(')');
                    return;
                }
                case Intrinsic::TextureSample: case Intrinsic::TextureSampleLevel: case Intrinsic::TextureSampleCmp:
                case Intrinsic::TextureSampleCmpLevelZero:
                {
                    static constexpr const char* kNames[] = {"Sample", "SampleLevel", "SampleCmp", "SampleCmpLevelZero"};
                    u32 which = e.intrinsic == Intrinsic::TextureSample ? 0 : e.intrinsic == Intrinsic::TextureSampleLevel ? 1 : e.intrinsic == Intrinsic::TextureSampleCmp ? 2 : 3;
                    Value(e.args[0]);
                    FormatAppend(out, ".{}(", kNames[which]);
                    Value(e.args[0]);
                    out.Append("_sampler");
                    for (usize i = 1; i < e.args.Size(); i++)
                    {
                        out.Append(", ");
                        Value(e.args[i]);
                    }
                    out.Append(')');
                    return;
                }
                case Intrinsic::TextureLoad:
                {
                    // Coordinates and the mip level in one vector.
                    u32 n = fn->exprs[e.args[1]].type.rows + 1;
                    Value(e.args[0]);
                    FormatAppend(out, ".Load(int{}(", n);
                    Value(e.args[1]);
                    out.Append(", ");
                    Value(e.args[2]);
                    out.Append("))");
                    return;
                }
                case Intrinsic::TextureSize: case Intrinsic::ImageSize:
                {
                    u32 n = e.type.rows;
                    const char* dims = n == 2 ? "uint w, h; t.GetDimensions(w, h); return int2(w, h);" : "uint w, h, d; t.GetDimensions(w, h, d); return int3(w, h, d);";
                    Global g = m.globals[fn->exprs[e.args[0]].index];
                    String resource = TypeOfResource(g);
                    Helper(Format("int{} hx_size({} t)\n{{\n    {}\n}}\n", n, resource, dims));
                    Call("hx_size", e);
                    return;
                }
                case Intrinsic::BufferLength:
                {
                    Global g = m.globals[fn->exprs[e.args[0]].index];
                    Helper(Format("int hx_length({} b)\n{{\n    uint count, stride;\n    b.GetDimensions(count, stride);\n    return int(count);\n}}\n",
                                  TypeOfResource(g)));
                    Call("hx_length", e);
                    return;
                }
                case Intrinsic::InterlockedAdd:
                {
                    Global g = m.globals[fn->exprs[e.args[0]].index];
                    String element = TypeName(g.element);
                    Helper(Format("{} hx_interlocked_add({} b, int i, {} v)\n{{\n    {} original;\n    InterlockedAdd(b[i], v, original);\n    return original;\n}}\n",
                                  element, TypeOfResource(g), element, element));
                    Call("hx_interlocked_add", e);
                    return;
                }
                case Intrinsic::BindlessSample: case Intrinsic::BindlessSampleLevel: case Intrinsic::BindlessLoad: case Intrinsic::BindlessStore:
                case Intrinsic::BindlessSize:
                {
                    // Table lookups of the RHI, written out only in the bytecode.
                    String name(IntrinsicName(e.intrinsic));
                    for (usize i = 0; i < name.Size(); i++)
                        if (name[i] == '.')
                            name[i] = '_';
                    Call(Format("hx_{}", name).CStr(), e);
                    return;
                }
                default: Call(IntrinsicName(e.intrinsic), e); return;
                }
            }

            String TypeOfResource(const Global& g) const
            {
                switch (g.type.base)
                {
                case BaseType::Texture2D: return Format("Texture2D<{}>", TypeName(g.element));
                case BaseType::Texture2DArray: return Format("Texture2DArray<{}>", TypeName(g.element));
                case BaseType::Texture3D: return Format("Texture3D<{}>", TypeName(g.element));
                case BaseType::TextureCube: return Format("TextureCube<{}>", TypeName(g.element));
                case BaseType::Image: return Format("RWTexture2D<{}>", TypeName(g.element));
                case BaseType::Buffer: return Format("{}StructuredBuffer<{}>", g.writable ? "RW" : "", TypeName(g.element));
                default: return TypeName(g.type);
                }
            }

            void Value(ExprId id)
            {
                const Expr& e = fn->exprs[id];
                switch (e.kind)
                {
                case ExprKind::ConstFloat: Number(e.number); break;
                case ExprKind::ConstInt:
                    if (e.type.base == BaseType::UInt)
                        FormatAppend(out, "{}u", u32(e.integer));
                    else if (e.integer < 0)
                        FormatAppend(out, "({})", e.integer);
                    else
                        FormatAppend(out, "{}", e.integer);
                    break;
                case ExprKind::ConstBool: out.Append(e.integer ? "true" : "false"); break;
                case ExprKind::Local: out.Append(Ident(fn->locals[e.index].name)); break;
                case ExprKind::Param: FormatAppend(out, "p_{}", Ident(fn->params[e.index].name)); break;
                case ExprKind::Global:
                    FormatAppend(out, "g_{}", m.globals[e.index].name);
                    break;
                case ExprKind::Field:
                    Value(e.a);
                    FormatAppend(out, ".m_{}", Ident(m.structs[fn->exprs[e.a].type.structIndex].fields[e.index].name));
                    break;
                case ExprKind::Swizzle:
                    Value(e.a);
                    out.Append('.');
                    for (u32 i = 0; i < e.swizzleCount; i++)
                        out.Append("xyzw"[e.swizzle[i] & 3]);
                    break;
                case ExprKind::Index:
                    Value(e.a);
                    out.Append('[');
                    Value(e.b);
                    out.Append(']');
                    break;
                case ExprKind::Unary:
                    out.Append(e.op == Op::Neg ? "(-" : e.op == Op::Not ? "(!" : "(~");
                    Value(e.a);
                    out.Append(')');
                    break;
                case ExprKind::Binary:
                {
                    static constexpr const char* kOperators[] = {"-", "!", "~", "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "&&", "||",
                                                                 "==", "!=", "<", "<=", ">", ">="};
                    out.Append('(');
                    Value(e.a);
                    FormatAppend(out, " {} ", kOperators[u32(e.op)]);
                    Value(e.b);
                    out.Append(')');
                    break;
                }
                case ExprKind::Cast:
                    FormatAppend(out, "(({})", TypeName(e.type));
                    Value(e.a);
                    out.Append(')');
                    break;
                case ExprKind::Construct:
                    if (e.args.Empty())
                    {
                        FormatAppend(out, "(({})0)", TypeName(e.type));
                        break;
                    }
                    if (e.type.base == BaseType::Struct || e.type.base == BaseType::Array)
                    {
                        // A struct value from its fields: HLSL has no constructors for structs.
                        out.Append('{');
                        Args(e);
                        out.Append('}');
                        break;
                    }
                    FormatAppend(out, "{}(", TypeName(e.type));
                    Args(e);
                    out.Append(')');
                    break;
                case ExprKind::Call:
                    FormatAppend(out, "{}(", Ident(m.functions[e.index].name));
                    Args(e);
                    out.Append(')');
                    break;
                case ExprKind::Intrinsic: WriteIntrinsic(e); break;
                }
            }

            void Line(StringView text)
            {
                out.Append(' ', indent * 4);
                out.Append(text);
                out.Append('\n');
            }

            void Statements(const Array<Stmt>& stmts)
            {
                for (const Stmt& s : stmts)
                {
                    out.Append(' ', indent * 4);
                    switch (s.kind)
                    {
                    case StmtKind::Assign:
                        Value(s.a);
                        out.Append(" = ");
                        Value(s.b);
                        out.Append(";\n");
                        break;
                    case StmtKind::Evaluate:
                        Value(s.a);
                        out.Append(";\n");
                        break;
                    case StmtKind::Return:
                        if (s.a)
                        {
                            out.Append("return ");
                            Value(s.a);
                            out.Append(";\n");
                        }
                        else
                            out.Append("return;\n");
                        break;
                    case StmtKind::Discard: out.Append("discard;\n"); break;
                    case StmtKind::Break: out.Append("break;\n"); break;
                    case StmtKind::Continue: out.Append("continue;\n"); break;
                    case StmtKind::Loop:
                        out.Append(fn->loops == LoopHint::Keep ? "[loop] while (true)\n" : "while (true)\n");
                        Line("{");
                        indent++;
                        Statements(s.body);
                        indent--;
                        Line("}");
                        break;
                    case StmtKind::If:
                        out.Append("if (");
                        Value(s.a);
                        out.Append(")\n");
                        Line("{");
                        indent++;
                        Statements(s.body);
                        indent--;
                        Line("}");
                        if (!s.elseBody.Empty())
                        {
                            Line("else");
                            Line("{");
                            indent++;
                            Statements(s.elseBody);
                            indent--;
                            Line("}");
                        }
                        break;
                    }
                }
            }

            void WriteFunction(const Function& f, bool entry)
            {
                fn = &f;
                if (entry && m.stage == Stage::Compute)
                    FormatAppend(out, "[numthreads({}, {}, {})]\n", m.groupSize[0], m.groupSize[1], m.groupSize[2]);
                FormatAppend(out, "{} {}(", TypeName(f.returnType), entry ? String("main") : Ident(f.name));
                static constexpr const char* kModes[] = {"in ", "out ", "inout "};
                for (usize i = 0; i < f.params.Size(); i++)
                {
                    const Variable& p = f.params[i];
                    FormatAppend(out, "{}{}{}", i ? ", " : "", kModes[u32(p.mode)], Declare(p.type, Format("p_{}", Ident(p.name))));
                    if (entry)
                        WriteSemantic(p.semantic, p.semanticIndex, true);
                }
                out.Append(')');
                if (entry && f.returnType.base != BaseType::Struct)
                    WriteSemantic(m.returnSemantic, m.returnSemanticIndex, false);
                out.Append("\n{\n");
                indent = 1;
                for (const Variable& v : f.locals)
                    Line(Format("{};", Declare(v.type, Ident(v.name))));
                Statements(f.body);
                out.Append("}\n\n");
            }

            bool Run()
            {
                static constexpr const char* kStages[] = {"vertex", "fragment", "compute"};
                FormatAppend(out, "// HLSL of the {} shader, for reading; the Direct3D backend writes the bytecode itself.\n", kStages[u32(m.stage)]);
                for (usize i = 0; i < m.structs.Size(); i++)
                {
                    FormatAppend(out, "struct {}\n{{\n", TypeName(Type::Struct(u32(i))));
                    for (const StructField& f : m.structs[i].fields)
                    {
                        if (f.unread)
                            continue;
                        FormatAppend(out, "    {}{}", f.type.IsMatrix() ? "row_major " : "", Declare(f.type, Format("m_{}", Ident(f.name))));
                        WriteSemantic(f.semantic, f.semanticIndex, false);
                        out.Append(";\n");
                    }
                    out.Append("};\n");
                }
                for (const Global& g : m.globals)
                {
                    switch (g.kind)
                    {
                    case GlobalKind::Uniform:
                        FormatAppend(out, "cbuffer hx_block_{} : register(b{})\n{{\n    {} g_{};\n}};\n", g.name, g.slot, TypeName(g.type), g.name);
                        break;
                    case GlobalKind::Texture:
                        FormatAppend(out, "{} g_{} : register(t{});\n{} g_{}_sampler : register(s{});\n", TypeOfResource(g), g.name, g.slot,
                                     g.comparison ? "SamplerComparisonState" : "SamplerState", g.name, g.slot);
                        break;
                    case GlobalKind::Storage:
                        FormatAppend(out, "{} g_{} : register({}{});\n", TypeOfResource(g), g.name, g.writable || g.type.base == BaseType::Image ? "u" : "t",
                                     g.slot);
                        break;
                    case GlobalKind::Sampler: break;
                    }
                }
                String body;
                {
                    Writer inner(m, body, error);
                    for (usize i = 0; i < m.functions.Size(); i++)
                        inner.WriteFunction(m.functions[i], i == m.entry);
                    for (String& h : inner.helpers)
                        Helper(Move(h));
                }
                if (!helpers.Empty())
                    out.Append("// Helpers for members HLSL has as statements; hx_bindless_ calls stand for the table lookups of the RHI.\n");
                for (const String& h : helpers)
                    out.Append(h);
                out.Append('\n');
                out.Append(body);
                return error.Empty();
            }
        };
    }

    bool EmitHlsl(const Module& module, String& out, String& error)
    {
        out.Clear();
        error.Clear();
        Writer w(module, out, error);
        return w.Run();
    }
}
