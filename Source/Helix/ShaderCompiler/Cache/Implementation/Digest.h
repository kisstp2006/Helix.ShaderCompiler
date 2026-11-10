#pragma once

#include <Helix/Core/Hashing/Md5.h>
#include <Helix/ShaderCompiler/IR/IR.h>

namespace helix::shadercompiler
{
    namespace cache
    {
        // Feeds a module to the digest field by field, so padding bytes never take part.
        struct Digest
        {
            Md5 md5;

            void U64(u64 v) { md5.Update(&v, 8); }
            void Text(StringView s)
            {
                U64(s.Size());
                md5.Update(s.Data(), s.Size());
            }
            void Add(Type t) { U64(u64(t.base) | (u64(t.rows) << 8) | (u64(t.cols) << 16) | (u64(t.half) << 24) | (u64(t.structIndex) << 32)); }
            void Add(const Variable& v)
            {
                Text(v.name);
                Add(v.type);
                U64(u64(v.mode) | (u64(v.semantic) << 8) | (u64(v.flat) << 16) | (u64(v.semanticIndex) << 32));
            }
            // Expressions are hashed by content from the statements that use them: what is left in the table of a
            // function after branches were removed does not count, so variants with the same code get the same key.
            void Add(const Function& f, ExprId id)
            {
                if (!id)
                    return U64(0);
                const Expr& e = f.exprs[id];
                U64(1 | (u64(e.kind) << 8) | (u64(e.op) << 16) | (u64(e.intrinsic) << 24) | (u64(e.swizzleCount) << 32) | (u64(e.swizzle[0]) << 40) |
                    (u64(e.swizzle[1]) << 42) | (u64(e.swizzle[2]) << 44) | (u64(e.swizzle[3]) << 46));
                Add(e.type);
                U64(e.index);
                U64(u64(e.integer));
                md5.Update(&e.number, 8);
                Add(f, e.a);
                Add(f, e.b);
                U64(e.args.Size());
                for (ExprId arg : e.args)
                    Add(f, arg);
            }
            void Add(const Function& f, const Array<Stmt>& stmts)
            {
                U64(stmts.Size());
                for (const Stmt& s : stmts)
                {
                    U64(u64(s.kind));
                    Add(f, s.a);
                    Add(f, s.b);
                    Add(f, s.body);
                    Add(f, s.elseBody);
                }
            }
            void Add(const Function& f)
            {
                Text(f.name);
                Add(f.returnType);
                U64(u64(f.loops));
                U64(f.params.Size());
                for (const Variable& v : f.params)
                    Add(v);
                U64(f.locals.Size());
                for (const Variable& v : f.locals)
                    Add(v);
                Add(f, f.body);
            }
        };
    }
}
