#include <Support/TestAssemblies.h>

#include <Helix/Core/Strings/Format.h>
#include <Helix/RHI/Device/Device.h>
#include <Helix/ShaderCompiler/Backends/Dxbc.h>
#include <Helix/ShaderCompiler/Backends/Glsl.h>
#include <Helix/ShaderCompiler/Cache/ShaderCache.h>
#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/Testing/Test.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace helix;
using namespace helix::rhi;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

namespace
{
    struct Reader
    {
        Span<const u8> data;
        usize at = 0;

        u32 U32()
        {
            u32 v = 0;
            if (at + 4 <= data.Size())
                std::memcpy(&v, data.Data() + at, 4);
            at += 4;
            return v;
        }

        const u8* Bytes(usize count)
        {
            const u8* p = at + count <= data.Size() ? data.Data() + at : nullptr;
            at += count;
            return p;
        }
    };

    // Every kernel of Tests/Shaders/Kernels is compiled, dispatched on the device and its buffers are compared byte by
    // byte with what the same C# code left on the CPU.
    void RunKernels(Device& d, bool direct3d)
    {
        if (!d.Caps().compute || !d.Caps().storageBuffers)
        {
            std::printf("    %s: no compute shaders, skipped\n", d.Caps().renderer);
            return;
        }
        bool es = d.Caps().backend == Backend::OpenGLES;
        Assembly a;
        HX_REQUIRE(a.Load(TestShadersImage()));
        Reader r{ShaderExpectations()};
        r.U32();
        r.U32();
        r.at = r.U32();
        u32 kernelCount = r.U32();
        HX_REQUIRE(kernelCount >= 3);
        u32 passed = 0;
        u32 skipped = 0;
        ShaderCache cache;
        for (u32 k = 0; k < kernelCount; k++)
        {
            u32 nameLength = 0;
            if (const u8* p = r.Bytes(2))
                nameLength = u32(p[0]) | (u32(p[1]) << 8);
            const u8* nameBytes = r.Bytes(nameLength);
            HX_REQUIRE(nameBytes != nullptr);
            String name(StringView(reinterpret_cast<const char*>(nameBytes), nameLength));
            // Keyword values of this run; the label names them.
            Array<String> keywordNames;
            Array<KeywordValue> keywords;
            String label = name;
            u32 keywordCount = r.U32();
            for (u32 i = 0; i < keywordCount; i++)
            {
                u32 length = 0;
                if (const u8* p = r.Bytes(2))
                    length = u32(p[0]) | (u32(p[1]) << 8);
                const u8* text = r.Bytes(length);
                HX_REQUIRE(text != nullptr);
                keywordNames.Push(String(StringView(reinterpret_cast<const char*>(text), length)));
                keywords.Push({StringView(), i64(i32(r.U32()))});
                label = helix::Format("{} {}={}", label, keywordNames.Back(), keywords.Back().value);
            }
            for (usize i = 0; i < keywords.Size(); i++)
                keywords[i].name = keywordNames[i];
            u32 groups[3] = {r.U32(), r.U32(), r.U32()};

            CommandList cl;
            Array<TransientAllocation> uniforms;
            Array<u32> uniformSlots;
            u32 uniformCount = r.U32();
            for (u32 u = 0; u < uniformCount; u++)
            {
                u32 slot = r.U32(), bytes = r.U32();
                const u8* data = r.Bytes(bytes);
                HX_REQUIRE(data != nullptr);
                uniforms.Push(d.AllocateTransient(bytes));
                uniformSlots.Push(slot);
                std::memcpy(uniforms.Back().data, data, bytes);
            }
            struct Bound
            {
                BufferHandle buffer;
                const u8* expected;
                u32 size;
                u32 slot;
            };
            Array<Bound> buffers;
            u32 bufferCount = r.U32();
            for (u32 b = 0; b < bufferCount; b++)
            {
                u32 slot = r.U32(), writable = r.U32();
                r.U32(); // element size, not needed: the views are raw
                u32 bytes = r.U32();
                const u8* before = r.Bytes(bytes);
                const u8* after = writable ? r.Bytes(bytes) : nullptr;
                HX_REQUIRE(before != nullptr && (!writable || after != nullptr));
                BufferHandle buffer = d.CreateBuffer({.size = bytes, .usage = BufferStorage, .initialData = before});
                HX_REQUIRE(buffer.IsValid());
                buffers.Push({buffer, after, bytes, slot});
            }
            struct Image
            {
                TextureHandle texture;
                const u8* expected;
                u32 width, height, slot, size;
                // Rounded to 8 bits on both sides: the last bit may differ; other formats must be equal bit for bit.
                bool rounded;
            };
            Array<Image> images;
            u32 imageCount = r.U32();
            for (u32 i = 0; i < imageCount; i++)
            {
                u32 slot = r.U32(), width = r.U32(), height = r.U32(), format = r.U32(), bytes = r.U32();
                const u8* before = r.Bytes(bytes);
                const u8* after = r.Bytes(bytes);
                static constexpr rhi::Format kFormats[] = {rhi::Format::Unknown,    rhi::Format::RGBA8Unorm, rhi::Format::RGBA16Float, rhi::Format::RGBA32Float,
                                                           rhi::Format::R32Float,   rhi::Format::R32Uint,    rhi::Format::R32Sint};
                HX_REQUIRE(before && after && format >= 1 && format <= 6);
                TextureHandle texture = d.CreateTexture({.format = kFormats[format], .width = width, .height = height, .usage = TextureStorage | TextureSampled,
                                                         .initialData = before});
                HX_REQUIRE(texture.IsValid());
                images.Push({texture, after, width, height, slot, bytes, format == 1});
            }
            struct Heap
            {
                BufferHandle buffer;
                const u8* expected;
                u32 size, index;
            };
            Array<Heap> heap;
            bool skip = false;
            u32 heapCount = r.U32();
            for (u32 i = 0; i < heapCount; i++)
            {
                u32 bytes = r.U32();
                const u8* before = r.Bytes(bytes);
                const u8* after = r.Bytes(bytes);
                HX_REQUIRE(before && after);
                skip = skip || !d.Caps().bindlessBuffers || es;
                if (skip)
                    continue;
                BufferHandle buffer = d.CreateBuffer({.size = bytes, .usage = BufferStorage, .bindless = true, .initialData = before});
                HX_REQUIRE(buffer.IsValid());
                heap.Push({buffer, after, bytes, d.RegisterBindlessBuffer(buffer)});
                HX_CHECK_EQ(heap.Back().index, i);
            }
            if (skip)
            {
                std::printf("    %s: no bindless buffers on %s, skipped\n", label.CStr(), d.Caps().renderer);
                for (const Bound& b : buffers)
                    d.Destroy(b.buffer);
                for (const Image& image : images)
                    d.Destroy(image.texture);
                skipped++;
                continue;
            }

            Module module;
            CompiledShader compiled;
            String error;
            u32 type = a.FindType("Helix.ShaderTests.Kernels", name);
            HX_REQUIRE(type != 0);
            TranslateOptions options;
            options.keywords = Span<const KeywordValue>(keywords.Data(), keywords.Size());
            options.passes.verify = true;
            // Through the cache, as the renderer compiles them; variants with the same code share an entry.
            bool ok = TranslateEntryPoint(a, a.FindMethod(type, "Run"), Stage::Compute, module, error, options) &&
                      CompileModule(module, direct3d ? ShaderTarget::Dxbc : es ? ShaderTarget::Essl310 : ShaderTarget::Glsl430, compiled, error, &cache);
            if (!ok)
                std::printf("    %s: %s\n", label.CStr(), error.CStr());
            // HELIX_PRINT_SHADERS=<kernel name> shows the GLSL or writes the bytecode to <kernel name>.dxbc.
            const char* print = std::getenv("HELIX_PRINT_SHADERS");
            if (ok && print && name == StringView(print))
            {
                String path = helix::Format("{}.dxbc", name);
                std::FILE* f = direct3d ? std::fopen(path.CStr(), "wb") : nullptr;
                if (f)
                {
                    std::fwrite(compiled.code.Data(), 1, compiled.code.Size(), f);
                    std::fclose(f);
                }
                if (!direct3d)
                    std::printf("%s\n", String(compiled.Text()).CStr());
            }
            ShaderHandle shader;
            PipelineHandle pipeline;
            if (ok)
            {
                Array<ShaderBinding> bindings;
                for (const GlslBinding& b : compiled.bindings)
                    bindings.Push({b.name, b.kind == GlobalKind::Uniform ? BindingKind::UniformBuffer : BindingKind::Texture, b.slot});
                ShaderDesc desc;
                desc.stage = ShaderStage::Compute;
                desc.source = direct3d ? StringView() : compiled.Text();
                desc.bytecode = direct3d ? compiled.code.Data() : nullptr;
                desc.bytecodeSize = direct3d ? compiled.code.Size() : 0;
                desc.bindings = bindings.Data();
                desc.bindingCount = u32(bindings.Size());
                shader = d.CreateShader(desc);
                pipeline = shader.IsValid() ? d.CreateComputePipeline({shader, "kernel"}) : PipelineHandle{};
                ok = pipeline.IsValid();
                if (!ok)
                    std::printf("    %s: the generated shader was rejected by the driver:\n%s\n", label.CStr(), direct3d ? "" : String(compiled.Text()).CStr());
            }
            if (ok)
            {
                cl.SetPipeline(pipeline);
                for (usize u = 0; u < uniforms.Size(); u++)
                    cl.SetUniformBuffer(uniformSlots[u], uniforms[u]);
                for (const Bound& b : buffers)
                    cl.SetStorageBuffer(b.slot, b.buffer, b.expected ? StorageAccess::ReadWrite : StorageAccess::ReadOnly);
                for (const Image& image : images)
                    cl.SetStorageTexture(image.slot, image.texture);
                cl.Dispatch(groups[0], groups[1], groups[2]);
                d.Submit(cl);
                for (const Image& image : images)
                {
                    Array<u8> pixels;
                    pixels.Resize(image.size);
                    u32 wrong = 0, first = 0;
                    if (d.ReadTexture(image.texture, 0, pixels.Data(), pixels.Size()))
                    {
                        for (usize i = 0; i < pixels.Size(); i++)
                        {
                            u32 diff = u32(pixels[i] > image.expected[i] ? pixels[i] - image.expected[i] : image.expected[i] - pixels[i]);
                            if (diff > (image.rounded ? 1u : 0u) && !wrong++)
                                first = u32(i);
                        }
                    }
                    else
                        wrong = 1;
                    if (wrong)
                    {
                        std::printf("    %s (%s): texture in slot %u has %u wrong bytes, the first at byte %u: GPU %u, CPU %u\n", label.CStr(),
                                    direct3d ? "DXBC" : es ? "ESSL 310" : "GLSL 430", image.slot, wrong, first, u32(pixels[first]), u32(image.expected[first]));
                        ok = false;
                    }
                }
                for (const Heap& h : heap)
                {
                    Array<u8> bytes;
                    bytes.Resize(h.size);
                    u32 wrong = 0, first = 0;
                    if (d.ReadBuffer(h.buffer, 0, bytes.Data(), h.size))
                    {
                        for (u32 i = 0; i < h.size; i++)
                            if (bytes[i] != h.expected[i] && !wrong++)
                                first = i;
                    }
                    else
                        wrong = 1;
                    if (wrong)
                    {
                        std::printf("    %s (%s): bindless buffer %u has %u wrong bytes, the first at byte %u: GPU %u, CPU %u\n", label.CStr(),
                                    direct3d ? "DXBC" : es ? "ESSL 310" : "GLSL 430", h.index, wrong, first, u32(bytes[first]), u32(h.expected[first]));
                        ok = false;
                    }
                }
                for (const Bound& b : buffers)
                {
                    if (!b.expected)
                        continue;
                    Array<u8> bytes;
                    bytes.Resize(b.size);
                    u32 wrong = 0, first = 0;
                    if (d.ReadBuffer(b.buffer, 0, bytes.Data(), b.size))
                    {
                        for (u32 i = 0; i < b.size; i++)
                            if (bytes[i] != b.expected[i] && !wrong++)
                                first = i;
                    }
                    else
                        wrong = 1;
                    if (wrong)
                    {
                        std::printf("    %s (%s): buffer in slot %u has %u wrong bytes, the first at byte %u: GPU %u, CPU %u\n", label.CStr(),
                                    direct3d ? "DXBC" : es ? "ESSL 310" : "GLSL 430", b.slot, wrong, first, u32(bytes[first]), u32(b.expected[first]));
                        ok = false;
                    }
                }
            }
            d.Destroy(pipeline);
            d.Destroy(shader);
            for (const Bound& b : buffers)
                d.Destroy(b.buffer);
            for (const Image& image : images)
                d.Destroy(image.texture);
            for (usize i = heap.Size(); i-- > 0;)
            {
                d.UnregisterBindlessBuffer(heap[i].index);
                d.Destroy(heap[i].buffer);
            }
            HX_CHECK(ok);
            passed += ok ? 1 : 0;
        }
        std::printf("    %u of %u kernels match the CPU on %s (%s)\n", passed, kernelCount - skipped, d.Caps().renderer, direct3d ? "Direct3D bytecode" : es ? "ESSL 310" : "GLSL 430");
    }
}

// ESSL 3.10 on an OpenGL ES 3.1 or 3.2 context, where the driver has one.
HX_TEST(Compute, EsslMatchesTheCpu)
{
    Device* d = Device::IsBackendAvailable(Backend::OpenGLES) ? Device::Create({.backend = Backend::OpenGLES, .debug = std::getenv("HELIX_GL_DEBUG") != nullptr, .vsync = false}) : nullptr;
    if (!d)
    {
        std::printf("    no OpenGL ES here, skipped\n");
        return;
    }
    RunKernels(*d, false);
    Device::Destroy(d);
}

HX_TEST(Compute, GlslMatchesTheCpu)
{
    Device* d = Device::IsBackendAvailable(Backend::OpenGL) ? Device::Create({.backend = Backend::OpenGL, .vsync = false}) : nullptr;
    if (!d)
    {
        std::printf("    no OpenGL here, skipped\n");
        return;
    }
    RunKernels(*d, false);
    Device::Destroy(d);
}

HX_TEST(Compute, DxbcMatchesTheCpu)
{
    if (!Device::IsBackendAvailable(Backend::D3D11))
    {
        std::printf("    no Direct3D 11 here, skipped\n");
        return;
    }
    char name[128];
    u32 ran = 0;
    for (u32 adapter = 0; Device::AdapterName(Backend::D3D11, adapter, name, sizeof(name)); adapter++)
    {
        Device* d = Device::Create({.backend = Backend::D3D11, .adapter = i32(adapter), .vsync = false});
        if (!d)
            continue;
        RunKernels(*d, true);
        Device::Destroy(d);
        ran++;
    }
    if (!ran)
    {
        Device* d = Device::Create({.backend = Backend::D3D11, .vsync = false});
        HX_REQUIRE(d != nullptr);
        RunKernels(*d, true);
        Device::Destroy(d);
    }
}
