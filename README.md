# Helix.ShaderCompiler

Shader compiler of the Helix engine. Shaders are written in C# with Helix.Shaders and compiled by the C# compiler; this
library reads the resulting assembly and turns the shader methods into GPU code. It contains no third party code.

Current state: vertex and fragment shaders compile to GLSL 3.30, GLSL 4.50, GLSL ES 3.00 and Direct3D 11 bytecode
(shader model 4.0); compute shaders and storage buffers compile to GLSL 4.30, GLSL ES 3.10 and shader model 5.0 bytecode.

## Layout

| Path | Content |
|---|---|
| `Source/Helix/ShaderCompiler/Assembly/` | `Assembly` (PE file, CLI metadata, method bodies, custom attributes), signatures, IL instruction decoder |
| `Source/Helix/ShaderCompiler/IR/` | The module a shader is translated to: types, structs, expressions, structured statements, functions, resources |
| `Source/Helix/ShaderCompiler/Frontend/` | `TranslateEntryPoint`: IL of an entry point and everything it calls to a module |
| `Source/Helix/ShaderCompiler/Definition/` | `ReadShaderDefinitions`: shaders, material properties, keywords, passes and render state declared in an assembly |
| `Source/Helix/ShaderCompiler/Transforms/` | `Simplify`: constant folding, constant locals, removal of code, functions and resources that cannot be reached |
| `Source/Helix/ShaderCompiler/Backends/` | `EmitGlsl`: a module to GLSL source with its binding table; `EmitDxbc`: a module to a Direct3D bytecode container |
| `Tests/Shaders/` | C# test shaders, built with the .NET SDK during the CMake build and embedded into the test program |
| `Tests/Oracle/` | C# program that runs the test shaders on the CPU and writes the images the GPU must reproduce |
| `Tests/Execution/` | Compiles every case, draws it through Helix.RHI and compares it with the CPU image |
| `Tests/Shaders/Rhi/` | C# shaders of the Helix.RHI tests |
| `Tools/ExportShaders/` | `helix_export_shaders`: compiles the shaders of a namespace to a C++ header (GLSL, GLSL ES, Direct3D bytecode, bindings) |
| `Tools/EmbedFile.cmake` | Turns a binary file into a C++ byte array |

## Compiling a shader

```cpp
using namespace helix::shadercompiler;
Assembly assembly;
assembly.Load(bytes);
u32 type = assembly.FindType("Game.Shaders", "Unlit");

Module module;
GlslShader shader;
String error;
if (!TranslateEntryPoint(assembly, assembly.FindMethod(type, "Fragment"), Stage::Fragment, module, error) ||
    !EmitGlsl(module, GlslTarget::Glsl330, shader, error))
    Report(error);
// shader.source is the GLSL text, shader.bindings maps the names in it to the slots given in C#.

DxbcShader code;
EmitDxbc(module, code, error);
// code.bytes goes to ID3D11Device::CreatePixelShader as it is.
```

## Shader definitions

A class derived from `ShaderDefinition` declares a shader. `ReadShaderDefinitions` reads all of them from an assembly
without translating code:

```cpp
Array<ShaderDescription> shaders;
if (!ReadShaderDefinitions(assembly, shaders, error))
    Report(error);
const ShaderPass* pass = shaders[0].FindPass("Forward");
// pass->state: culling, depth, blending per target, stencil, color mask, depth offset.
TranslateEntryPoint(assembly, pass->fragment, Stage::Fragment, module, error, options);
```

A `ShaderDescription` holds the name, fallback, tags and LOD of the shader, its material properties (type, display name,
range, default value or default texture, flags such as main texture or HDR, the keyword a toggle drives), its keywords
(scope, shader feature, dynamic, the members of an enum keyword with their values) and its passes in source order with
their render state and entry point methods. Declarations that cannot work are errors naming the shader and the member: a
property type materials cannot hold, a pass without both entry points, a toggle naming a keyword the shader does not have,
a name used twice.

## Compute shaders and storage buffers

```csharp
public static class Integrate
{
    [Storage(1)] public static StructuredBuffer<Particle> source;
    [Storage(2)] public static RWStructuredBuffer<Particle> moved;

    [ComputeShader(64)]
    public static void Run([DispatchThreadId] uint3 id)
    {
        Particle p = source[(int)id.x];
        p.position.xy += p.velocity;
        moved[(int)id.x] = p;
    }
}
```

`TranslateEntryPoint(..., Stage::Compute, ...)` reads the work group size from `[ComputeShader(x, y, z)]`. Buffers are
`StructuredBuffer<T>` (read) and `RWStructuredBuffer<T>` (read and write) static fields with `[Storage(slot)]`, used with
the indexer and `Length`; they work in vertex and fragment shaders too where the target has storage buffers. Elements
are numbers, vectors, matrices or structs whose fields lie the same way in C#, in std430 and packed: no three component
vectors or bools, no holes; the error names the field.

| C# | GLSL 4.30 / ES 3.10 | Direct3D (shader model 5.0) |
|---|---|---|
| `[Storage(n)] RWStructuredBuffer<T> name` | `layout(std430, binding = n) buffer hx_buffer_name { T g_name[]; }` | raw buffer view `u<n>` |
| `[Storage(n)] StructuredBuffer<T> name` | the same, `readonly` | raw buffer view `t<16 + n>` |
| `[Storage(n, Format = f)] RWTexture2D name` | `layout(f, binding = n) writeonly uniform image2D g_name` | typed view `u<n>` |
| `image[texel] = color` | `imageStore` | `store_uav_typed` |
| `InterlockedAdd(buffer, index, value)` | `atomicAdd` | `imm_atomic_iadd` |
| `[DispatchThreadId] uint3` | `gl_GlobalInvocationID` | `SV_DispatchThreadID` |

Direct3D buffers are read and written through raw (byte address) views, with the element size multiplied in by the
generated code. That way every buffer can be bound, also one that is a vertex buffer or holds indirect draw arguments,
which cannot be structured buffers. A storage texture can only be written; `InterlockedAdd` works on buffers of `int` or
`uint` and returns the value before the add.

## Keywords and variants

A `[Keyword]` field (a `bool` or an enum of the shader assembly) is a constant while one variant is compiled:

```cpp
const KeywordValue keywords[] = {{"Fog", 1}, {"Quality", 2}};
TranslateOptions options;
options.keywords = keywords;
TranslateEntryPoint(assembly, method, Stage::Fragment, module, error, options);
// module.keywords: the keywords this code read, with the values used.
```

Keywords that are not listed are off. `if (Fog)`, `switch (Quality)` and conditions built from them are decided at compile
time: the module holds only the branch taken, and functions, uniform blocks and textures used by other variants only are
not in it. Names that the shader does not have are ignored, so one global keyword set can be passed to every shader.
`module.keywords` tells which keywords can change the code at all; variants that differ in other keywords are identical.

The C# method

```csharp
[FragmentShader]
[return: Target(0)]
public static float4 Fragment(Varyings input)
{
    float4 texel = albedo.Sample(albedoSampler, input.uv);
    float3 color = texel.xyz * draw.tint.xyz;
    if (texel.w < 0.5f)
        Discard();
    return new float4(color, texel.w);
}
```

becomes

```glsl
vec4 f_Fragment_0(in S_Varyings_0 p_input)
{
    vec4 l0;
    vec3 l1;
    l0 = texture(g_albedo, p_input.m_uv);
    l1 = (l0.xyz * g_draw.m_tint.xyz);
    if ((l0.w < 0.5))
    {
        discard;
    }
    return vec4(l1, l0.w);
}
```

## How it works

1. The assembly reader finds the method, its signature and its IL.
2. The frontend runs the IL on a symbolic stack: every instruction builds an expression, stores become statements. Calls into
   the shader library turn into operations (`ShaderMath.Dot` is `dot`, `float3.xyz` is a swizzle, `float4 * float4` is a
   multiplication); other static methods are translated the same way and become functions, generic ones once per type.
3. The branches of the IL form a control flow graph. Short circuit conditions are merged back into `&&` and `||`, loops are
   found from back edges, and the graph is written as `if`, `while`, `break`, `continue` and `return`.
4. The backend prints the module as GLSL and checks that every uniform struct has the same layout in C# and in std140.
   The Direct3D backend writes instructions for a register machine instead: every variable gets registers, expressions
   are evaluated into temporaries, calls are copied into the caller (a `return` becomes a `break` out of a loop that runs
   once), and the result is wrapped in a container with its signatures and checksum.

## What shader code may use

- `float`, `int`, `uint`, `bool`, the vector and matrix types of Helix.Shaders, structs of those (also nested).
- Static methods, generic methods, `ref`, `out` and `in` parameters, methods of user structs.
- `if`, `for`, `while`, `do`, `switch`, `break`, `continue`, early `return`, `?:`, `&&`, `||`.
- The functions of `ShaderMath`, `Texture2D.Sample`, `SampleLevel`, `Load`, `Size`, and uniform structs, textures and samplers
  declared as static fields.
- `double` is computed as `float`.

Not available, each reported with the method name and the reason: classes and anything allocated, recursion, exceptions,
`goto`, 64 bit integers, reading a storage texture, calls into other libraries.
Dynamic keywords (decided at run time) are not available yet.

## Conventions of the generated GLSL

| C# | GLSL |
|---|---|
| vertex input `[Location(n)]` | `layout(location = n) in ... hx_in<n>` |
| value from vertex to fragment `[Location(n)]` | `hx_v<n>`, `flat` for integers |
| fragment output `[Target(n)]` | `layout(location = n) out ... hx_target<n>` |
| `[Position]` | `gl_Position` in the vertex stage, `gl_FragCoord` in the fragment stage |
| `[Uniform(slot)] static T name` | `layout(std140) uniform hx_block_name { T g_name; }` |
| `[Texture(slot)] static Texture2D name` | `uniform sampler2D g_name`; the sampler state travels with the slot |

## Conventions of the generated Direct3D bytecode

| C# | Direct3D |
|---|---|
| vertex input `[Location(n)]` | `TEXCOORD<n>`, register n |
| value from vertex to fragment `[Location(n)]` | `TEXCOORD<n>`, register n + 1; integers and `Flat` are not interpolated |
| fragment output `[Target(n)]` | `SV_Target<n>` |
| `[Position]` | `SV_Position`, register 0 |
| `[VertexId]`, `[InstanceId]` | `SV_VertexID`, `SV_InstanceID` |
| `[Uniform(slot)] static T name` | constant buffer `b<slot>` |
| `[Texture(slot)] static Texture2D name` | texture `t<slot>` with sampler `s<slot>` |

Not written yet for Direct3D: front facing input, depth output, writing a vector or matrix element with a computed index.
`asin`, `acos` and `atan` are polynomial approximations (error below 0.001), since the bytecode has no such instructions.

## Tests

- Assembly reader: tables, signatures, instructions and attributes of real assemblies, and 400 randomly damaged files.
- Execution: 17 C# shaders (arithmetic, branches, loops, `continue`, early returns from nested loops, switch, functions
  with `ref`/`out` and generics, library functions, integers, matrices, uniforms, textures, discard, bool vectors, nested
  structs) run on the CPU as C# and on the GPU as generated GLSL 3.30 and 4.50 and as Direct3D bytecode; the images must
  agree within 2/255. They run on the OpenGL device and on every Direct3D 11 adapter the machine offers.
- Direct3D container: chunks, sizes, signatures and registers of both stages, and that output is deterministic.
- GLSL ES 3.00 output is checked as text only; it is not executed yet.
- Compute: five kernels (a uniform and a buffer, structs between two buffers on a 2D grid, an in place update with a
  helper that loops and returns early on a 3D grid, counters shared through atomic adds, a storage texture) run on the CPU and are dispatched as GLSL 4.30 and as Direct3D
  bytecode on every adapter; the buffers must be the same byte by byte, texels within the last bit.
- Keywords: a case with a bool and an enum keyword runs in all six combinations on the CPU and the GPU; the text tests
  check that the code, the helper function and the texture of other variants are gone.
- Definitions: a shader using every declarative feature is read back field by field, a pass of it is compiled with
  keyword values taken from the description, and three broken definitions give their messages.
- Rejected shaders: recursion, a uniform struct whose layout differs between C# and std140, a missing stage attribute.

`HELIX_PRINT_SHADERS=<case name>` prints the generated fragment shader of one execution case and writes its bytecode to
`<case name>.dxbc` in the working directory.

## Shaders of the Helix.RHI tests

Helix.RHI sits below this library and cannot compile shaders in its own build, so its tests use shaders compiled here.
The target `export_rhi_shaders` (part of the default build on desktop) writes `Generated/CompiledShaders.h` into the build
directory from `Tests/Shaders/Rhi`. After changing those shaders, copy the file to `Tests/Support/CompiledShaders.h` of
Helix.RHI and run its tests.

## Build

```
git clone https://github.com/kisstp2006/Helix.ShaderCompiler
git -C Helix.ShaderCompiler submodule update --init
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

The tests need the .NET 11 SDK (`dotnet` on the path, or `HELIX_DOTNET_ROOT` or `DOTNET_ROOT` pointing to it).

## License

GPL-3.0, see `LICENSE`.
