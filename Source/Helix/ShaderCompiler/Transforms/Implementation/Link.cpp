#include <Helix/ShaderCompiler/Transforms/Link.h>

#include <Helix/Core/Strings/Format.h>

namespace helix::shadercompiler
{
    namespace
    {
        // Marks the locations of the fragment inputs its code reads: a parameter read at all, or the fields of a
        // struct parameter that are read (all of them when the struct is read as a whole).
        void Reads(const Module& m, const Function& f, ExprId id, bool fieldBase, Array<u32>& out)
        {
            if (!id)
                return;
            const Expr& e = f.exprs[id];
            if (e.kind == ExprKind::Field && f.exprs[e.a].kind == ExprKind::Param)
            {
                const Variable& p = f.params[f.exprs[e.a].index];
                const StructField& field = m.structs[p.type.structIndex].fields[e.index];
                if (field.semantic == Semantic::Location)
                    out.Push(field.semanticIndex);
            }
            else if (e.kind == ExprKind::Param && !fieldBase)
            {
                const Variable& p = f.params[e.index];
                if (p.semantic == Semantic::Location)
                    out.Push(p.semanticIndex);
                if (p.type.base == BaseType::Struct)
                    for (const StructField& field : m.structs[p.type.structIndex].fields)
                        if (field.semantic == Semantic::Location)
                            out.Push(field.semanticIndex);
            }
            Reads(m, f, e.a, e.kind == ExprKind::Field, out);
            Reads(m, f, e.b, false, out);
            for (ExprId arg : e.args)
                Reads(m, f, arg, false, out);
        }

        void Reads(const Module& m, const Function& f, const Array<Stmt>& stmts, Array<u32>& out)
        {
            for (const Stmt& s : stmts)
            {
                Reads(m, f, s.a, false, out);
                Reads(m, f, s.b, false, out);
                Reads(m, f, s.body, out);
                Reads(m, f, s.elseBody, out);
            }
        }
    }

    bool LinkStages(Module& vertex, const Module& fragment, String& error)
    {
        error.Clear();
        if (vertex.stage != Stage::Vertex || fragment.stage != Stage::Fragment)
        {
            error = "LinkStages takes a vertex and a fragment module";
            return false;
        }
        Array<u32> read;
        const Function& entry = fragment.functions[fragment.entry];
        Reads(fragment, entry, entry.body, read);
        // What the vertex stage writes.
        Array<u32> written;
        Type r = vertex.functions[vertex.entry].returnType;
        if (r.base == BaseType::Struct)
            for (const StructField& f : vertex.structs[r.structIndex].fields)
                if (f.semantic == Semantic::Location)
                    written.Push(f.semanticIndex);
        if (r.base != BaseType::Struct && vertex.returnSemantic == Semantic::Location)
            written.Push(vertex.returnSemanticIndex);
        for (u32 location : read)
            if (!written.Contains(location))
            {
                error = Format("the fragment stage reads location {}, which the vertex stage does not write", location);
                return false;
            }
        if (r.base == BaseType::Struct)
            for (StructField& f : vertex.structs[r.structIndex].fields)
                f.unread = f.semantic == Semantic::Location && !read.Contains(f.semanticIndex);
        return true;
    }
}
