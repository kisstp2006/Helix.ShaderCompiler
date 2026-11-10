#include <Support/TestAssemblies.h>

#include <Helix/Core/Strings/Format.h>
#include <Helix/RHI/Device/Device.h>
#include <Helix/ShaderCompiler/Backends/Dxbc.h>
#include <Helix/ShaderCompiler/Backends/Glsl.h>
#include <Helix/ShaderCompiler/Frontend/Frontend.h>
#include <Helix/ShaderCompiler/Reflection/Reflection.h>
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
    // Sequential reader of the oracle file.
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

    ShaderHandle MakeShader(Device& device, const GlslShader& shader, ShaderStage stage)
    {
        Array<ShaderBinding> bindings;
        for (const GlslBinding& b : shader.bindings)
            bindings.Push({b.name, b.kind == GlobalKind::Uniform ? BindingKind::UniformBuffer : BindingKind::Texture, b.slot});
        ShaderDesc desc;
        desc.stage = stage;
        desc.source = shader.source;
        desc.bindings = bindings.Data();
        desc.bindingCount = u32(bindings.Size());
        return device.CreateShader(desc);
    }

    // The texture formats of the oracle file, by code.
    rhi::Format TestFormat(u32 code)
    {
        static constexpr rhi::Format kFormats[] = {rhi::Format::RGBA8Unorm, rhi::Format::R32Float,  rhi::Format::RG32Float,
                                                   rhi::Format::R32Uint,    rhi::Format::RG32Uint,  rhi::Format::RGBA32Uint,
                                                   rhi::Format::R32Sint,    rhi::Format::RG32Sint,  rhi::Format::RGBA32Sint,
                                                   rhi::Format::D32Float};
        return code < 10 ? kFormats[code] : rhi::Format::Unknown;
    }

    ShaderHandle MakeShader(Device& device, const DxbcShader& shader, ShaderStage stage)
    {
        ShaderDesc desc;
        desc.stage = stage;
        desc.bytecode = shader.bytes.Data();
        desc.bytecodeSize = shader.bytes.Size();
        return device.CreateShader(desc);
    }

    void RunCases(Device& d, bool direct3d);
}

// Every case of Tests/Shaders/Cases is compiled from its IL to GLSL, drawn on the GPU and compared with the image the same
// C# code produced on the CPU.
HX_TEST(Execution, GlslMatchesTheCpu)
{
    Device* d = Device::IsBackendAvailable(Backend::OpenGL) ? Device::Create({.backend = Backend::OpenGL, .vsync = false}) : nullptr;
    if (!d)
    {
        std::printf("    no OpenGL here, skipped\n");
        return;
    }
    RunCases(*d, false);
    Device::Destroy(d);
}

// The same through the Direct3D bytecode backend, on every adapter of the machine.
HX_TEST(Execution, DxbcMatchesTheCpu)
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
        RunCases(*d, true);
        Device::Destroy(d);
        ran++;
    }
    if (!ran)
    {
        Device* d = Device::Create({.backend = Backend::D3D11, .vsync = false});
        HX_REQUIRE(d != nullptr);
        RunCases(*d, true);
        Device::Destroy(d);
    }
}

namespace
{
void RunCases(Device& d, bool direct3d)
{
    Assembly a, library, includes;
    HX_REQUIRE(a.Load(TestShadersImage()) && library.Load(ShaderLibraryImage()) && includes.Load(ShaderIncludesImage()));
    const Assembly* references[] = {&library, &includes};

    Reader r{ShaderExpectations()};
    u32 size = r.U32(), caseCount = r.U32();
    r.U32();
    HX_REQUIRE(size == 16 && caseCount >= 10);

    const f32 triangle[] = {-1, -1, 3, -1, -1, 3};
    BufferHandle vertices = d.CreateBuffer({.size = sizeof(triangle), .usage = BufferVertex, .initialData = triangle});
    TextureHandle target = d.CreateTexture({.format = Format::RGBA8Unorm, .width = size, .height = size, .usage = TextureRenderTarget | TextureSampled});
    TextureHandle depthTarget = d.CreateTexture({.format = Format::D32Float, .width = size, .height = size, .usage = TextureDepthStencil});
    u32 fullscreen = a.FindType("Helix.ShaderTests.Cases", "Fullscreen");
    HX_REQUIRE(fullscreen != 0);

    // Desktop GLSL 3.30 always, 4.50 as well where the driver has it.
    GlslTarget targets[2] = {GlslTarget::Glsl330, GlslTarget::Glsl450};
    u32 targetCount = !direct3d && (d.Caps().apiMajor > 4 || (d.Caps().apiMajor == 4 && d.Caps().apiMinor >= 5)) ? 2 : 1;
    u32 fullTargetCount = targetCount, skipped = 0;
    u32 passed = 0;
    for (u32 c = 0; c < caseCount; c++)
    {
        u32 nameLength = 0;
        if (const u8* p = r.Bytes(2))
            nameLength = u32(p[0]) | (u32(p[1]) << 8);
        const u8* nameBytes = r.Bytes(nameLength);
        const u8* expected = r.Bytes(usize(size) * size * 4);
        HX_REQUIRE(nameBytes && expected);
        String name(StringView(reinterpret_cast<const char*>(nameBytes), nameLength));

        CommandList bind;
        Array<TextureHandle> textures;
        Array<SamplerHandle> samplers;
        u32 uniformCount = r.U32();
        for (u32 u = 0; u < uniformCount; u++)
        {
            u32 slot = r.U32(), bytes = r.U32();
            const u8* data = r.Bytes(bytes);
            HX_REQUIRE(data != nullptr);
            TransientAllocation block = d.AllocateTransient(bytes);
            std::memcpy(block.data, data, bytes);
            bind.SetUniformBuffer(slot, block);
        }
        u32 textureCount = r.U32();
        for (u32 t = 0; t < textureCount; t++)
        {
            u32 slot = r.U32(), width = r.U32(), height = r.U32(), depth = r.U32(), kind = r.U32(), filter = r.U32(), address = r.U32();
            u32 compare = r.U32(), format = r.U32(), bytes = r.U32();
            const u8* texels = r.Bytes(bytes);
            HX_REQUIRE(texels != nullptr && TestFormat(format) != rhi::Format::Unknown && kind < 4);
            const TextureType kTypes[] = {TextureType::Texture2D, TextureType::Texture2DArray, TextureType::Texture3D, TextureType::TextureCube};
            textures.Push(d.CreateTexture({.type = kTypes[kind], .format = TestFormat(format), .width = width, .height = height,
                                           .depthOrLayers = kind == 3 ? 1u : depth, .initialData = texels}));
            HX_REQUIRE(textures.Back().IsValid());
            Filter f = filter == 1 ? Filter::Linear : Filter::Nearest;
            AddressMode mode = address == 1 ? AddressMode::ClampToEdge : AddressMode::Repeat;
            samplers.Push(d.CreateSampler({.minFilter = f, .magFilter = f, .mipFilter = Filter::Nearest, .addressU = mode, .addressV = mode, .addressW = mode,
                                           .compare = compare != 0, .compareFunction = CompareFunction(compare ? compare - 1 : 0)}));
            bind.SetTexture(slot, textures.Back(), samplers.Back());
        }

        // The variant of the case: its keyword values, also shown in messages.
        Array<String> keywordNames;
        Array<KeywordValue> keywords;
        u32 keywordCount = r.U32();
        String label = name;
        for (u32 k = 0; k < keywordCount; k++)
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
        for (usize k = 0; k < keywords.Size(); k++)
            keywords[k].name = keywordNames[k];
        TranslateOptions options;
        options.keywords = Span<const KeywordValue>(keywords.Data(), keywords.Size());
        options.references = Span<const Assembly* const>(references, 2);

        // Bindless resources, registered in the order of their CPU indices; on a fresh table the indices match.
        Array<TextureHandle> bindlessTextures;
        Array<BufferHandle> bindlessBuffers;
        Array<u32> bindlessIndices, bufferIndices;
        bool skip = false;
        u32 bindlessTextureCount = r.U32();
        for (u32 t = 0; t < bindlessTextureCount; t++)
        {
            u32 width = r.U32(), height = r.U32(), depth = r.U32(), kind = r.U32(), filter = r.U32(), address = r.U32();
            u32 compare = r.U32(), format = r.U32(), bytes = r.U32();
            const u8* pixels = r.Bytes(bytes);
            HX_REQUIRE(pixels != nullptr && format == 0 && kind == 0 && depth == 1 && compare == 0);
            skip = skip || !d.Caps().bindlessTextures;
            if (skip)
                continue;
            bindlessTextures.Push(d.CreateTexture({.format = Format::RGBA8Unorm, .width = width, .height = height, .bindless = true, .initialData = pixels}));
            Filter f = filter == 1 ? Filter::Linear : Filter::Nearest;
            AddressMode mode = address == 1 ? AddressMode::ClampToEdge : AddressMode::Repeat;
            samplers.Push(d.CreateSampler({.minFilter = f, .magFilter = f, .mipFilter = Filter::Nearest, .addressU = mode, .addressV = mode}));
            bindlessIndices.Push(d.RegisterBindless(bindlessTextures.Back(), samplers.Back()));
            HX_CHECK_EQ(bindlessIndices.Back(), t);
        }
        u32 bindlessBufferCount = r.U32();
        for (u32 b = 0; b < bindlessBufferCount; b++)
        {
            u32 bytes = r.U32();
            const u8* data = r.Bytes(bytes);
            HX_REQUIRE(data != nullptr);
            skip = skip || !d.Caps().bindlessBuffers;
            if (skip)
                continue;
            bindlessBuffers.Push(d.CreateBuffer({.size = bytes, .usage = BufferStorage, .bindless = true, .initialData = data}));
            bufferIndices.Push(d.RegisterBindlessBuffer(bindlessBuffers.Back()));
            HX_CHECK_EQ(bufferIndices.Back(), b);
        }

        u32 type = a.FindType("Helix.ShaderTests.Cases", name);
        HX_REQUIRE(type != 0);
        bool caseOk = true;
        if (skip)
        {
            std::printf("    %s: no bindless resources on %s, skipped\n", label.CStr(), d.Caps().renderer);
            skipped++;
            targetCount = 0;
        }
        for (u32 version = 0; version < targetCount && caseOk; version++)
        {
            Module vertexModule, fragmentModule, probeModule;
            GlslShader vertexShader, fragmentShader, probeShader;
            DxbcShader vertexCode, fragmentCode, probeCode;
            String error;
            bool compiled = TranslateEntryPoint(a, a.FindMethod(fullscreen, "Vertex"), Stage::Vertex, vertexModule, error) &&
                            TranslateEntryPoint(a, a.FindMethod(type, "Fragment"), Stage::Fragment, fragmentModule, error, options) &&
                            TranslateEntryPoint(a, a.FindMethod(fullscreen, "Probe"), Stage::Fragment, probeModule, error);
            // A front facing input is drawn twice, once per winding; a depth output gets a depth buffer and a probe.
            ShaderReflection fragmentInfo;
            bool frontFaces = false, writesDepth = false;
            compiled = compiled && Reflect(fragmentModule, fragmentInfo, error);
            for (const ReflectedValue& v : fragmentInfo.inputs)
                frontFaces = frontFaces || v.semantic == Semantic::FrontFacing;
            for (const ReflectedValue& v : fragmentInfo.outputs)
                writesDepth = writesDepth || v.semantic == Semantic::Depth;
            // Bindless textures exist from GLSL 4.50 on, with the extension.
            if (compiled && !direct3d && targets[version] == GlslTarget::Glsl330 && FindBindless(fragmentModule).textures)
                continue;
            if (compiled && direct3d)
                compiled = EmitDxbc(vertexModule, vertexCode, error) && EmitDxbc(fragmentModule, fragmentCode, error) && EmitDxbc(probeModule, probeCode, error);
            else if (compiled)
                compiled = EmitGlsl(vertexModule, targets[version], vertexShader, error) && EmitGlsl(fragmentModule, targets[version], fragmentShader, error) &&
                           EmitGlsl(probeModule, targets[version], probeShader, error);
            if (!compiled)
            {
                std::printf("    %s: %s\n", label.CStr(), error.CStr());
                caseOk = false;
                break;
            }
            // HELIX_PRINT_SHADERS=<case name> shows the generated fragment shader of one case; bytecode goes to <case>.dxbc
            // and Fullscreen.dxbc in the working directory.
            const char* print = std::getenv("HELIX_PRINT_SHADERS");
            if (print && name == StringView(print) && version == 0)
            {
                if (direct3d)
                {
                    String path = helix::Format("{}.dxbc", name);
                    const DxbcShader* dumps[] = {&fragmentCode, &vertexCode};
                    const char* paths[] = {path.CStr(), "Fullscreen.dxbc"};
                    for (int i = 0; i < 2; i++)
                        if (std::FILE* f = std::fopen(paths[i], "wb"))
                        {
                            std::fwrite(dumps[i]->bytes.Data(), 1, dumps[i]->bytes.Size(), f);
                            std::fclose(f);
                        }
                }
                else
                    std::printf("%s\n", fragmentShader.source.CStr());
            }
            ShaderHandle vs = direct3d ? MakeShader(d, vertexCode, ShaderStage::Vertex) : MakeShader(d, vertexShader, ShaderStage::Vertex);
            ShaderHandle fs = direct3d ? MakeShader(d, fragmentCode, ShaderStage::Fragment) : MakeShader(d, fragmentShader, ShaderStage::Fragment);
            PipelineDesc pd;
            pd.vertexShader = vs;
            pd.fragmentShader = fs;
            pd.vertexBuffers[0] = {8, StepMode::Vertex};
            pd.vertexBufferCount = 1;
            pd.attributes[0] = {0, 0, VertexFormat::Float2, 0};
            pd.attributeCount = 1;
            if (writesDepth)
            {
                pd.depthTest = true;
                pd.depthWrite = true;
                pd.depthCompare = CompareFunction::Less;
                pd.depthFormat = Format::D32Float;
            }
            PipelineHandle pipeline = vs.IsValid() && fs.IsValid() ? d.CreatePipeline(pd) : PipelineHandle{};
            PipelineDesc flipped = pd;
            flipped.frontCounterClockwise = true;
            PipelineHandle frontPipeline = frontFaces ? d.CreatePipeline(flipped) : PipelineHandle{};
            ShaderHandle probe = !writesDepth ? ShaderHandle{} : direct3d ? MakeShader(d, probeCode, ShaderStage::Fragment) : MakeShader(d, probeShader, ShaderStage::Fragment);
            PipelineDesc probeDesc = pd;
            probeDesc.fragmentShader = probe;
            probeDesc.depthWrite = false;
            PipelineHandle probePipeline = writesDepth ? d.CreatePipeline(probeDesc) : PipelineHandle{};
            if ((frontFaces && !frontPipeline.IsValid()) || (writesDepth && !probePipeline.IsValid()))
                pipeline = PipelineHandle{};
            if (!pipeline.IsValid())
            {
                std::printf("    %s: the generated shader was rejected by the driver:\n%s\n", label.CStr(), fragmentShader.source.CStr());
                caseOk = false;
            }
            else
            {
                CommandList cl;
                RenderPassDesc pass;
                pass.colors[0].texture = target;
                pass.colors[0].clearColor[3] = 0.0f;
                if (writesDepth)
                    pass.depth.texture = depthTarget;
                cl.BeginRenderPass(pass);
                cl.SetPipeline(pipeline);
                cl.SetVertexBuffer(0, vertices);
                d.Submit(cl);
                d.Submit(bind);
                // Dynamic keywords: their values go into the keyword block the compiler added.
                if (const ReflectedBlock* block = fragmentInfo.FindBlock(kKeywordBlockName))
                {
                    TransientAllocation values = d.AllocateTransient(block->size);
                    std::memset(values.data, 0, block->size);
                    for (const ReflectedMember& member : block->members)
                        for (const KeywordValue& k : keywords)
                            if (k.name == StringView(member.name))
                            {
                                u32 value = u32(k.value);
                                std::memcpy(static_cast<u8*>(values.data) + member.offset, &value, 4);
                            }
                    CommandList keywordBind;
                    keywordBind.SetUniformBuffer(block->slot, values);
                    d.Submit(keywordBind);
                }
                CommandList draw;
                if (frontFaces)
                {
                    draw.SetScissor({0, 0, size / 2, size});
                    draw.Draw(3);
                    draw.SetPipeline(frontPipeline);
                    draw.SetScissor({i32(size / 2), 0, size / 2, size});
                }
                draw.Draw(3);
                if (writesDepth)
                {
                    draw.SetPipeline(probePipeline);
                    draw.Draw(3);
                }
                draw.EndRenderPass();
                d.Submit(draw);
                Array<u8> pixels;
                pixels.Resize(usize(size) * size * 4);
                u32 wrong = 0, worst = 0, firstWrong = 0;
                if (d.ReadTexture(target, 0, pixels.Data(), pixels.Size()))
                {
                    for (usize i = 0; i < pixels.Size(); i++)
                    {
                        u32 diff = u32(pixels[i] > expected[i] ? pixels[i] - expected[i] : expected[i] - pixels[i]);
                        if (diff > 2 && !wrong++)
                            firstWrong = u32(i);
                        worst = Max(worst, diff);
                    }
                }
                else
                    wrong = 1;
                if (wrong)
                {
                    u32 p = firstWrong / 4;
                    std::printf("    %s (%s): %u channels differ, worst by %u; first at pixel (%u, %u) channel %u: GPU %u, CPU %u\n", label.CStr(),
                                direct3d ? "DXBC" : version ? "GLSL 450" : "GLSL 330", wrong, worst, p % size, p / size, firstWrong % 4, u32(pixels[firstWrong]),
                                u32(expected[firstWrong]));
                    std::printf("%s\n", fragmentShader.source.CStr());
                    caseOk = false;
                }
            }
            d.Destroy(pipeline);
            d.Destroy(frontPipeline);
            d.Destroy(probePipeline);
            d.Destroy(probe);
            d.Destroy(vs);
            d.Destroy(fs);
        }
        // Released in reverse, so the next case gets the lowest indices again.
        for (usize i = bindlessIndices.Size(); i-- > 0;)
            d.UnregisterBindless(bindlessIndices[i]);
        for (usize i = bufferIndices.Size(); i-- > 0;)
            d.UnregisterBindlessBuffer(bufferIndices[i]);
        for (TextureHandle t : bindlessTextures)
            d.Destroy(t);
        for (BufferHandle b : bindlessBuffers)
            d.Destroy(b);
        for (TextureHandle t : textures)
            d.Destroy(t);
        for (SamplerHandle s : samplers)
            d.Destroy(s);
        targetCount = fullTargetCount;
        if (skip)
            continue;
        HX_CHECK(caseOk);
        passed += caseOk ? 1 : 0;
    }
    std::printf("    %u of %u shaders match the CPU on %s (%s)\n", passed, caseCount - skipped, d.Caps().renderer,
                direct3d ? "Direct3D bytecode" : targetCount == 2 ? "GLSL 330 and 450" : "GLSL 330");
    d.Destroy(target);
    d.Destroy(depthTarget);
    d.Destroy(vertices);
}
}
