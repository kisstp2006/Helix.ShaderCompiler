#include <Helix/ShaderCompiler/Backends/Glsl.h>
#include <Helix/ShaderCompiler/Backends/Implementation/UniformLayout.h>

#include <Helix/Core/Strings/Format.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace helix::shadercompiler
{
    namespace
    {
        struct Emitter
        {
            const Module& m;
            GlslTarget target;
            String& out;
            String& error;
            const Function* fn = nullptr;
            u32 indent = 0;

            bool Fail(StringView message)
            {
                if (error.Empty())
                    error = message;
                return false;
            }

            String TypeName(Type t) const
            {
                static constexpr const char* kScalar[] = {"void", "bool", "int", "uint", "float"};
                static constexpr const char* kVector[] = {"", "bvec", "ivec", "uvec", "vec"};
                if (t.base == BaseType::Struct)
                    return Format("S_{}_{}", m.structs[t.structIndex].name, u32(t.structIndex));
                if (t.base == BaseType::Texture2D)
                    return String("sampler2D");
                if (t.IsMatrix())
                    return Format("mat{}", u32(t.rows));
                if (t.rows > 1)
                    return Format("{}{}", kVector[u32(t.base)], u32(t.rows));
                return String(kScalar[u32(t.base)]);
            }

            void Line(StringView text)
            {
                out.Append(' ', indent * 4);
                out.Append(text);
                out.Append('\n');
            }

            // ---------------------------------------------------------------- expressions

            void Number(f64 value)
            {
                if (std::isnan(value))
                {
                    out.Append("(0.0 / 0.0)");
                    return;
                }
                if (std::isinf(value))
                {
                    out.Append(value > 0 ? "(1.0 / 0.0)" : "(-1.0 / 0.0)");
                    return;
                }
                char text[40];
                std::snprintf(text, sizeof(text), "%.9g", value);
                if (!std::strpbrk(text, ".e"))
                    std::strcat(text, ".0");
                if (value < 0)
                    FormatAppend(out, "({})", StringView(text));
                else
                    out.Append(text);
            }

            void Zero(Type t)
            {
                if (t.base == BaseType::Struct)
                {
                    const StructType& s = m.structs[t.structIndex];
                    out.Append(TypeName(t));
                    out.Append('(');
                    for (usize i = 0; i < s.fields.Size(); i++)
                    {
                        if (i)
                            out.Append(", ");
                        Zero(s.fields[i].type);
                    }
                    out.Append(')');
                    return;
                }
                const char* scalar = t.base == BaseType::Float ? "0.0" : t.base == BaseType::Bool ? "false" : t.base == BaseType::UInt ? "0u" : "0";
                if (t.IsScalar())
                    out.Append(scalar);
                else
                    FormatAppend(out, "{}({})", TypeName(t), scalar);
            }

            void Args(const Expr& e, usize first = 0)
            {
                for (usize i = first; i < e.args.Size(); i++)
                {
                    if (i > first)
                        out.Append(", ");
                    Value(e.args[i]);
                }
            }

            bool IntrinsicCall(const Expr& e)
            {
                auto call = [&](const char* name) {
                    out.Append(name);
                    out.Append('(');
                    Args(e);
                    out.Append(')');
                    return true;
                };
                Type first = e.args.Empty() ? kVoid : fn->exprs[e.args[0]].type;
                switch (e.intrinsic)
                {
                case Intrinsic::Round: return call("roundEven");
                case Intrinsic::Frac: return call("fract");
                case Intrinsic::Rsqrt: return call("inversesqrt");
                case Intrinsic::Atan2: return call("atan");
                case Intrinsic::Lerp: return call("mix");
                case Intrinsic::Ddx: return call("dFdx");
                case Intrinsic::Ddy: return call("dFdy");
                case Intrinsic::Saturate:
                    out.Append("clamp(");
                    Value(e.args[0]);
                    out.Append(", 0.0, 1.0)");
                    return true;
                case Intrinsic::Pow:
                {
                    // GLSL has no pow(vector, scalar).
                    Type second = fn->exprs[e.args[1]].type;
                    out.Append("pow(");
                    Value(e.args[0]);
                    out.Append(", ");
                    if (!(first == second))
                        FormatAppend(out, "{}(", TypeName(first));
                    Value(e.args[1]);
                    out.Append(first == second ? ")" : "))");
                    return true;
                }
                case Intrinsic::Fmod:
                    // Remainder with the sign of the dividend, as in C#; GLSL mod() follows the divisor.
                    out.Append('(');
                    Value(e.args[0]);
                    out.Append(" - ");
                    Value(e.args[1]);
                    out.Append(" * trunc(");
                    Value(e.args[0]);
                    out.Append(" / ");
                    Value(e.args[1]);
                    out.Append("))");
                    return true;
                case Intrinsic::Mul:
                    out.Append('(');
                    Value(e.args[0]);
                    out.Append(" * ");
                    Value(e.args[1]);
                    out.Append(')');
                    return true;
                case Intrinsic::Select:
                {
                    Type value = fn->exprs[e.args[1]].type;
                    if (first.IsScalar())
                    {
                        out.Append('(');
                        Value(e.args[0]);
                        out.Append(" ? ");
                        Value(e.args[1]);
                        out.Append(" : ");
                        Value(e.args[2]);
                        out.Append(')');
                    }
                    else if (value.base == BaseType::Float)
                    {
                        out.Append("mix(");
                        Value(e.args[2]);
                        out.Append(", ");
                        Value(e.args[1]);
                        FormatAppend(out, ", {}(", TypeName(value));
                        Value(e.args[0]);
                        out.Append("))");
                    }
                    else
                    {
                        // mix() with a bool vector needs GLSL 4.5 or ES 3.1 for integers, so pick per component.
                        out.Append(TypeName(value));
                        out.Append('(');
                        for (u32 c = 0; c < value.rows; c++)
                        {
                            if (c)
                                out.Append(", ");
                            const char* component[] = {".x", ".y", ".z", ".w"};
                            Value(e.args[0]);
                            FormatAppend(out, "{} ? ", component[c]);
                            Value(e.args[1]);
                            FormatAppend(out, "{} : ", component[c]);
                            Value(e.args[2]);
                            out.Append(component[c]);
                        }
                        out.Append(')');
                    }
                    return true;
                }
                case Intrinsic::TextureSample: return call("texture");
                case Intrinsic::TextureSampleLevel: return call("textureLod");
                case Intrinsic::TextureLoad: return call("texelFetch");
                case Intrinsic::TextureSize:
                    out.Append("textureSize(");
                    Value(e.args[0]);
                    out.Append(", 0)");
                    return true;
                case Intrinsic::BufferLength:
                    Value(e.args[0]);
                    out.Append(".length()");
                    return true;
                case Intrinsic::InterlockedAdd:
                    out.Append("atomicAdd(");
                    Value(e.args[0]);
                    out.Append('[');
                    Value(e.args[1]);
                    out.Append("], ");
                    Value(e.args[2]);
                    out.Append(')');
                    return true;
                default: return call(IntrinsicName(e.intrinsic));
                }
            }

            void Binary(const Expr& e)
            {
                Type ta = fn->exprs[e.a].type, tb = fn->exprs[e.b].type;
                bool vector = ta.IsVector() || tb.IsVector();
                auto function = [&](const char* name) {
                    out.Append(name);
                    out.Append('(');
                    Value(e.a);
                    out.Append(", ");
                    Value(e.b);
                    out.Append(')');
                };
                if (vector && e.op >= Op::Eq)
                {
                    static constexpr const char* kCompare[] = {"equal", "notEqual", "lessThan", "lessThanEqual", "greaterThan", "greaterThanEqual"};
                    function(kCompare[u32(e.op) - u32(Op::Eq)]);
                    return;
                }
                if (e.op == Op::Rem && ta.base == BaseType::Float)
                {
                    out.Append('(');
                    Value(e.a);
                    out.Append(" - ");
                    Value(e.b);
                    out.Append(" * trunc(");
                    Value(e.a);
                    out.Append(" / ");
                    Value(e.b);
                    out.Append("))");
                    return;
                }
                static constexpr const char* kOperators[] = {"-", "!", "~", "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "&&", "||",
                                                             "==", "!=", "<", "<=", ">", ">="};
                if (vector && ta.base == BaseType::Bool && (e.op == Op::And || e.op == Op::Or || e.op == Op::Xor))
                {
                    // GLSL has no logic operators on bool vectors; go through integers.
                    FormatAppend(out, "bvec{}(ivec{}(", u32(ta.rows), u32(ta.rows));
                    Value(e.a);
                    FormatAppend(out, ") {} ivec{}(", kOperators[u32(e.op)], u32(ta.rows));
                    Value(e.b);
                    out.Append("))");
                    return;
                }
                out.Append('(');
                Value(e.a);
                FormatAppend(out, " {} ", kOperators[u32(e.op)]);
                Value(e.b);
                out.Append(')');
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
                case ExprKind::Local: out.Append(fn->locals[e.index].name); break;
                case ExprKind::Param: FormatAppend(out, "p_{}", fn->params[e.index].name); break;
                case ExprKind::Global: FormatAppend(out, "g_{}", m.globals[e.index].name); break;
                case ExprKind::Field:
                    Value(e.a);
                    FormatAppend(out, ".m_{}", m.structs[fn->exprs[e.a].type.structIndex].fields[e.index].name);
                    break;
                case ExprKind::Swizzle:
                    Value(e.a);
                    out.Append('.');
                    for (u32 i = 0; i < e.swizzleCount; i++)
                        out.Append("xyzw"[e.swizzle[i]]);
                    break;
                case ExprKind::Index:
                    Value(e.a);
                    out.Append('[');
                    Value(e.b);
                    out.Append(']');
                    break;
                case ExprKind::Unary:
                    if (e.op == Op::Not && fn->exprs[e.a].type.IsVector())
                    {
                        out.Append("not(");
                        Value(e.a);
                        out.Append(')');
                        break;
                    }
                    out.Append(e.op == Op::Neg ? "(-" : e.op == Op::Not ? "(!" : "(~");
                    Value(e.a);
                    out.Append(')');
                    break;
                case ExprKind::Binary: Binary(e); break;
                case ExprKind::Cast:
                    FormatAppend(out, "{}(", TypeName(e.type));
                    Value(e.a);
                    out.Append(')');
                    break;
                case ExprKind::Construct:
                    if (e.args.Empty())
                    {
                        Zero(e.type);
                        break;
                    }
                    FormatAppend(out, "{}(", TypeName(e.type));
                    Args(e);
                    out.Append(')');
                    break;
                case ExprKind::Call:
                    FormatAppend(out, "{}(", m.functions[e.index].name);
                    Args(e);
                    out.Append(')');
                    break;
                case ExprKind::Intrinsic: IntrinsicCall(e); break;
                }
            }

            // ---------------------------------------------------------------- statements

            void Statements(const Array<Stmt>& stmts)
            {
                for (const Stmt& s : stmts)
                {
                    out.Append(' ', indent * 4);
                    switch (s.kind)
                    {
                    case StmtKind::Assign:
                    {
                        const Expr& place = fn->exprs[s.a];
                        if (place.kind == ExprKind::Index && fn->exprs[place.a].type.base == BaseType::Image)
                        {
                            // A texel of a storage texture.
                            out.Append("imageStore(");
                            Value(place.a);
                            out.Append(", ");
                            Value(place.b);
                            out.Append(", ");
                            Value(s.b);
                            out.Append(");\n");
                            break;
                        }
                        Value(s.a);
                        out.Append(" = ");
                        Value(s.b);
                        out.Append(";\n");
                        break;
                    }
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
                        out.Append("while (true)\n");
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

            void FunctionBody(const Function& f)
            {
                fn = &f;
                FormatAppend(out, "{} {}(", TypeName(f.returnType), f.name);
                for (usize i = 0; i < f.params.Size(); i++)
                {
                    static constexpr const char* kModes[] = {"in", "out", "inout"};
                    FormatAppend(out, "{}{} {} p_{}", i ? ", " : "", kModes[u32(f.params[i].mode)], TypeName(f.params[i].type), f.params[i].name);
                }
                out.Append(")\n{\n");
                indent = 1;
                for (const Variable& v : f.locals)
                    Line(Format("{} {};", TypeName(v.type), v.name));
                Statements(f.body);
                indent = 0;
                out.Append("}\n\n");
            }

            // ---------------------------------------------------------------- stage interface

            // Global declaration (when needed) and the GLSL expression of one stage input or output.
            bool Interface(Semantic semantic, u32 index, Type type, bool flat, bool input, String& expression, StringView what)
            {
                bool vertex = m.stage == Stage::Vertex;
                const char* qualifier = (flat || type.IsInteger()) ? "flat " : "";
                String name;
                switch (semantic)
                {
                case Semantic::Location:
                    if (vertex && input)
                    {
                        name = Format("hx_in{}", index);
                        FormatAppend(out, "layout(location = {}) in {} {};\n", index, TypeName(type), name);
                    }
                    else if (vertex != input)
                    {
                        name = Format("hx_v{}", index);
                        FormatAppend(out, "{}{} {} {};\n", qualifier, input ? "in" : "out", TypeName(type), name);
                    }
                    else
                        return Fail(Format("{}: a fragment shader writes targets, not locations", what));
                    expression = name;
                    return true;
                case Semantic::Position:
                    if (vertex && !input)
                        expression = "gl_Position";
                    else if (!vertex && input)
                        expression = "gl_FragCoord";
                    else
                        return Fail(Format("{}: [Position] is a vertex output or a fragment input", what));
                    return true;
                case Semantic::Target:
                    if (vertex || input)
                        return Fail(Format("{}: [Target] is a fragment output", what));
                    name = Format("hx_target{}", index);
                    FormatAppend(out, "layout(location = {}) out {} {};\n", index, TypeName(type), name);
                    expression = name;
                    return true;
                case Semantic::Depth:
                    if (vertex || input)
                        return Fail(Format("{}: [Depth] is a fragment output", what));
                    expression = "gl_FragDepth";
                    return true;
                case Semantic::VertexId:
                case Semantic::InstanceId:
                    if (!vertex || !input)
                        return Fail(Format("{}: vertex and instance ids are vertex inputs", what));
                    expression = Format("{}({})", TypeName(type), semantic == Semantic::VertexId ? "gl_VertexID" : "gl_InstanceID");
                    return true;
                case Semantic::FrontFacing:
                    if (vertex || !input)
                        return Fail(Format("{}: [FrontFacing] is a fragment input", what));
                    expression = "gl_FrontFacing";
                    return true;
                case Semantic::DispatchThreadId:
                    if (m.stage != Stage::Compute || !input || type.rows != 3 || !type.IsInteger())
                        return Fail(Format("{}: [DispatchThreadId] is a uint3 or int3 input of a compute shader", what));
                    expression = Format("{}(gl_GlobalInvocationID)", TypeName(type));
                    return true;
                default: return Fail(Format("{} has no stage attribute such as [Location(n)] or [Position]", what));
                }
            }

            bool EntryPoint()
            {
                const Function& entry = m.functions[m.entry];
                String body;
                Array<String> arguments;
                for (usize i = 0; i < entry.params.Size(); i++)
                {
                    const Variable& p = entry.params[i];
                    if (p.mode != ParamMode::In)
                        return Fail(Format("entry point parameter {} is ref or out; return the outputs instead", p.name));
                    String expression;
                    if (p.type.base == BaseType::Struct)
                    {
                        String local = Format("a{}", i);
                        FormatAppend(body, "    {} {};\n", TypeName(p.type), local);
                        for (const StructField& f : m.structs[p.type.structIndex].fields)
                        {
                            if (!Interface(f.semantic, f.semanticIndex, f.type, f.flat, true, expression, Format("field {} of parameter {}", f.name, p.name)))
                                return false;
                            FormatAppend(body, "    {}.m_{} = {};\n", local, f.name, expression);
                        }
                        arguments.Push(Move(local));
                    }
                    else
                    {
                        if (!Interface(p.semantic, p.semanticIndex, p.type, p.flat, true, expression, Format("parameter {}", p.name)))
                            return false;
                        arguments.Push(Move(expression));
                    }
                }
                String call = Format("{}(", entry.name);
                for (usize i = 0; i < arguments.Size(); i++)
                    FormatAppend(call, "{}{}", i ? ", " : "", arguments[i]);
                call.Append(")");
                Type r = entry.returnType;
                String expression;
                if (r.IsVoid())
                    FormatAppend(body, "    {};\n", call);
                else if (r.base == BaseType::Struct)
                {
                    FormatAppend(body, "    {} r = {};\n", TypeName(r), call);
                    for (const StructField& f : m.structs[r.structIndex].fields)
                    {
                        if (!Interface(f.semantic, f.semanticIndex, f.type, f.flat, false, expression, Format("field {} of the return value", f.name)))
                            return false;
                        FormatAppend(body, "    {} = r.m_{};\n", expression, f.name);
                    }
                }
                else
                {
                    if (!Interface(m.returnSemantic, m.returnSemanticIndex, r, false, false, expression, "the return value"))
                        return false;
                    FormatAppend(body, "    {} = {};\n", expression, call);
                }
                if (m.stage == Stage::Compute)
                    FormatAppend(out, "\nlayout(local_size_x = {}, local_size_y = {}, local_size_z = {}) in;\n", m.groupSize[0], m.groupSize[1],
                                 m.groupSize[2]);
                FormatAppend(out, "\nvoid main()\n{{\n{}}}\n", body);
                return true;
            }

            bool Run(GlslShader& shader)
            {
                static constexpr const char* kHeaders[] = {
                    "#version 330 core\n",
                    "#version 450 core\n",
                    "#version 300 es\nprecision highp float;\nprecision highp int;\n",
                    "#version 430 core\n",
                    "#version 310 es\nprecision highp float;\nprecision highp int;\n",
                };
                out.Append(kHeaders[u32(target)]);
                bool modern = target == GlslTarget::Glsl430 || target == GlslTarget::Glsl450 || target == GlslTarget::Essl310;
                if (m.stage == Stage::Compute && !modern)
                    return Fail("compute shaders need GLSL 4.30 or GLSL ES 3.10");
                for (usize i = 0; i < m.structs.Size(); i++)
                {
                    FormatAppend(out, "struct {}\n{{\n", TypeName(Type::Struct(u32(i))));
                    for (const StructField& f : m.structs[i].fields)
                        FormatAppend(out, "    {} m_{};\n", TypeName(f.type), f.name);
                    out.Append("};\n");
                }
                for (const Global& g : m.globals)
                {
                    if (g.kind == GlobalKind::Uniform)
                    {
                        if (g.type.base != BaseType::Struct)
                            return Fail(Format("uniform {} must be a struct", g.name));
                        if (!CheckUniformLayout(m, g.type, g.name, 0, 0, error))
                            return false;
                        FormatAppend(out, "layout(std140) uniform hx_block_{}\n{{\n    {} g_{};\n}};\n", g.name, TypeName(g.type), g.name);
                        shader.bindings.Push({Format("hx_block_{}", g.name), GlobalKind::Uniform, g.slot});
                    }
                    else if (g.kind == GlobalKind::Storage)
                    {
                        if (!modern)
                            return Fail(Format("storage buffer {} needs GLSL 4.30 or GLSL ES 3.10", g.name));
                        if (g.type.base == BaseType::Image)
                        {
                            static constexpr const char* kFormats[] = {"rgba8", "rgba16f", "rgba32f", "r32f"};
                            FormatAppend(out, "layout({}, binding = {}) writeonly uniform highp image2D g_{};\n", kFormats[u32(g.format) & 3], g.slot, g.name);
                            continue;
                        }
                        if (!CheckBufferLayout(m, g.element, g.name, error))
                            return false;
                        FormatAppend(out, "layout(std430, binding = {}) {}buffer hx_buffer_{}\n{{\n    {} g_{}[];\n}};\n", g.slot,
                                     g.writable ? "" : "readonly ", g.name, TypeName(g.element), g.name);
                    }
                    else if (g.kind == GlobalKind::Texture)
                    {
                        FormatAppend(out, "uniform sampler2D g_{};\n", g.name);
                        shader.bindings.Push({Format("g_{}", g.name), GlobalKind::Texture, g.slot});
                    }
                }
                out.Append('\n');
                for (const Function& f : m.functions)
                    FunctionBody(f);
                return EntryPoint();
            }
        };
    }

    bool EmitGlsl(const Module& module, GlslTarget target, GlslShader& out, String& error)
    {
        out = GlslShader();
        error.Clear();
        Emitter e{module, target, out.source, error};
        return e.Run(out);
    }
}
