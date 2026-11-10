#include <Support/TestAssemblies.h>

#include <Helix/RHI/Device/Device.h>
#include <Helix/ShaderCompiler/Backends/Glsl.h>
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
}

// Every case of Tests/Shaders/Cases is compiled from its IL to GLSL, drawn on the GPU and compared with the image the same
// C# code produced on the CPU.
HX_TEST(Execution, CompiledShadersMatchTheCpu)
{
    if (!Device::IsBackendAvailable(Backend::OpenGL))
    {
        std::printf("    no OpenGL here, skipped\n");
        return;
    }
    Device* created = Device::Create({.backend = Backend::OpenGL, .vsync = false});
    if (!created)
    {
        std::printf("    OpenGL device creation failed, skipped\n");
        return;
    }
    Device& d = *created;
    Assembly a;
    HX_REQUIRE(a.Load(TestShadersImage()));

    Reader r{ShaderExpectations()};
    u32 size = r.U32(), caseCount = r.U32();
    HX_REQUIRE(size == 16 && caseCount >= 10);

    const f32 triangle[] = {-1, -1, 3, -1, -1, 3};
    BufferHandle vertices = d.CreateBuffer({.size = sizeof(triangle), .usage = BufferVertex, .initialData = triangle});
    TextureHandle target = d.CreateTexture({.format = Format::RGBA8Unorm, .width = size, .height = size, .usage = TextureRenderTarget | TextureSampled});
    u32 fullscreen = a.FindType("Helix.ShaderTests.Cases", "Fullscreen");
    HX_REQUIRE(fullscreen != 0);

    // Desktop GLSL 3.30 always, 4.50 as well where the driver has it.
    GlslTarget targets[2] = {GlslTarget::Glsl330, GlslTarget::Glsl450};
    u32 targetCount = d.Caps().apiMajor > 4 || (d.Caps().apiMajor == 4 && d.Caps().apiMinor >= 5) ? 2 : 1;
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
            u32 slot = r.U32(), width = r.U32(), height = r.U32(), filter = r.U32(), address = r.U32();
            const u8* pixels = r.Bytes(usize(width) * height * 4);
            HX_REQUIRE(pixels != nullptr);
            textures.Push(d.CreateTexture({.format = Format::RGBA8Unorm, .width = width, .height = height, .initialData = pixels}));
            Filter f = filter == 1 ? Filter::Linear : Filter::Nearest;
            AddressMode mode = address == 1 ? AddressMode::ClampToEdge : AddressMode::Repeat;
            samplers.Push(d.CreateSampler({.minFilter = f, .magFilter = f, .mipFilter = Filter::Nearest, .addressU = mode, .addressV = mode}));
            bind.SetTexture(slot, textures.Back(), samplers.Back());
        }

        u32 type = a.FindType("Helix.ShaderTests.Cases", name);
        HX_REQUIRE(type != 0);
        bool caseOk = true;
        for (u32 version = 0; version < targetCount && caseOk; version++)
        {
            Module vertexModule, fragmentModule;
            GlslShader vertexShader, fragmentShader;
            String error;
            bool compiled = TranslateEntryPoint(a, a.FindMethod(fullscreen, "Vertex"), Stage::Vertex, vertexModule, error) &&
                            EmitGlsl(vertexModule, targets[version], vertexShader, error) &&
                            TranslateEntryPoint(a, a.FindMethod(type, "Fragment"), Stage::Fragment, fragmentModule, error) &&
                            EmitGlsl(fragmentModule, targets[version], fragmentShader, error);
            if (!compiled)
            {
                std::printf("    %s: %s\n", name.CStr(), error.CStr());
                caseOk = false;
                break;
            }
            // HELIX_PRINT_SHADERS=<case name> shows the generated fragment shader of one case.
            const char* print = std::getenv("HELIX_PRINT_SHADERS");
            if (print && name == StringView(print) && version == 0)
                std::printf("%s\n", fragmentShader.source.CStr());
            ShaderHandle vs = MakeShader(d, vertexShader, ShaderStage::Vertex);
            ShaderHandle fs = MakeShader(d, fragmentShader, ShaderStage::Fragment);
            PipelineDesc pd;
            pd.vertexShader = vs;
            pd.fragmentShader = fs;
            pd.vertexBuffers[0] = {8, StepMode::Vertex};
            pd.vertexBufferCount = 1;
            pd.attributes[0] = {0, 0, VertexFormat::Float2, 0};
            pd.attributeCount = 1;
            PipelineHandle pipeline = vs.IsValid() && fs.IsValid() ? d.CreatePipeline(pd) : PipelineHandle{};
            if (!pipeline.IsValid())
            {
                std::printf("    %s: the generated GLSL was rejected by the driver:\n%s\n", name.CStr(), fragmentShader.source.CStr());
                caseOk = false;
            }
            else
            {
                CommandList cl;
                RenderPassDesc pass;
                pass.colors[0].texture = target;
                pass.colors[0].clearColor[3] = 0.0f;
                cl.BeginRenderPass(pass);
                cl.SetPipeline(pipeline);
                cl.SetVertexBuffer(0, vertices);
                d.Submit(cl);
                d.Submit(bind);
                CommandList draw;
                draw.Draw(3);
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
                    std::printf("    %s (%s): %u channels differ, worst by %u; first at pixel (%u, %u) channel %u: GPU %u, CPU %u\n", name.CStr(),
                                version ? "GLSL 450" : "GLSL 330", wrong, worst, p % size, p / size, firstWrong % 4, u32(pixels[firstWrong]),
                                u32(expected[firstWrong]));
                    std::printf("%s\n", fragmentShader.source.CStr());
                    caseOk = false;
                }
            }
            d.Destroy(pipeline);
            d.Destroy(vs);
            d.Destroy(fs);
        }
        for (TextureHandle t : textures)
            d.Destroy(t);
        for (SamplerHandle s : samplers)
            d.Destroy(s);
        HX_CHECK(caseOk);
        passed += caseOk ? 1 : 0;
    }
    std::printf("    %u of %u shaders match the CPU on %s (%u GLSL versions)\n", passed, caseCount, d.Caps().renderer, targetCount);
    d.Destroy(target);
    d.Destroy(vertices);
    Device::Destroy(created);
}
