#include <Helix/ShaderCompiler/Variants/Variants.h>

#include <Helix/Core/Strings/Format.h>
#include <Helix/Jobs/Jobs.h>
#include <Helix/ShaderCompiler/Transforms/Link.h>

namespace helix::shadercompiler
{
    namespace
    {
        struct Batch
        {
            const Assembly* assembly;
            const ShaderPass* pass;
            const VariantSet* set;
            ShaderTarget target;
            const TranslateOptions* options;
            ShaderCache* cache;
            PassVariant* out;
        };

        void CompileOne(const Batch& b, u32 variant)
        {
            PassVariant& result = b.out[variant];
            Array<KeywordValue> keywords;
            b.set->Values(variant, keywords);
            TranslateOptions options = *b.options;
            options.keywords = Span<const KeywordValue>(keywords.Data(), keywords.Size());
            const Assembly& code = b.pass->assembly ? *b.pass->assembly : *b.assembly;
            // The fragment stage first: the vertex stage is fitted to it, so its keys take the fragment key in.
            ShaderKey fragmentSource = MakeSourceKey(code, b.pass->fragment, Stage::Fragment, options, b.target);
            ShaderKey vertexSource = CombineKeys(MakeSourceKey(code, b.pass->vertex, Stage::Vertex, options, b.target), fragmentSource);
            bool fragmentCached = b.cache->Find(fragmentSource, result.fragment), vertexCached = b.cache->Find(vertexSource, result.vertex);
            if (fragmentCached && vertexCached)
                return;
            Module vertex, fragment;
            String error;
            if (!TranslateEntryPoint(code, b.pass->vertex, Stage::Vertex, vertex, error, options) ||
                !TranslateEntryPoint(code, b.pass->fragment, Stage::Fragment, fragment, error, options) || !LinkStages(vertex, fragment, error) ||
                (!fragmentCached && !CompileModule(fragment, b.target, result.fragment, error, b.cache)) ||
                (!vertexCached && !CompileModule(vertex, b.target, result.vertex, error, b.cache)))
            {
                result.error = Format("variant {}: {}", variant, error);
                return;
            }
            if (!fragmentCached)
                b.cache->Store(fragmentSource, result.fragment);
            if (!vertexCached)
                b.cache->Store(vertexSource, result.vertex);
        }
    }

    bool CompileVariants(const Assembly& assembly, const ShaderPass& pass, const VariantSet& set, ShaderTarget target, const TranslateOptions& options,
                         ShaderCache& cache, Array<PassVariant>& out, String& error)
    {
        error.Clear();
        out.Clear();
        out.Resize(set.variants.Size());
        Batch batch{&assembly, &pass, &set, target, &options, &cache, out.Data()};
        u32 count = u32(set.variants.Size());
        if (jobs::IsInitialized())
        {
            const Batch* shared = &batch;
            jobs::Complete(jobs::ParallelFor(count, 1, [shared](u32 i) { CompileOne(*shared, i); }));
        }
        else
            for (u32 i = 0; i < count; i++)
                CompileOne(batch, i);
        for (const PassVariant& v : out)
            if (!v.error.Empty())
            {
                error = v.error;
                return false;
            }
        return true;
    }
}
