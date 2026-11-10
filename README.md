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
| `Source/Helix/ShaderCompiler/Variants/` | `EnumerateVariants`: the keyword combinations a pass has to be compiled for |
| `Source/Helix/ShaderCompiler/Cache/` | `CompileModule` and `ShaderCache`: one entry point for all targets, results kept by content in memory and on disk |
| `Source/Helix/ShaderCompiler/Material/` | `BindProperties`: where each material property of a shader goes in one compiled pass, and the default block data |
| `Source/Helix/ShaderCompiler/Reflection/` | `Reflect` and `LinkReflection`: uniform block layouts, resource slots, stage inputs and outputs of a compiled variant, and whether two stages fit |
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

## Variants and the cache

```cpp
const i64 inUse[] = {2};
const KeywordSelection used[] = {{"Quality", inUse}};
VariantSet set;
EnumerateVariants(assembly, shader, *pass, used, set, error);

ShaderCache cache("Library/ShaderCache");
Array<KeywordValue> keywords;
for (usize v = 0; v < set.variants.Size(); v++)
{
    set.Values(v, keywords);
    TranslateOptions options;
    options.keywords = keywords;
    TranslateEntryPoint(assembly, pass->fragment, Stage::Fragment, module, error, options);
    CompileModule(module, ShaderTarget::Dxbc, compiled, error, &cache);
}
```

`EnumerateVariants` lists every combination of the values of the keywords the code of a pass reads; keywords it never
reads make no variants. A plain keyword takes all its values, a shader feature only its off value and the values
materials use (`used`), so unused variants are never built. More variants than the limit (4096 by default) is an error.

`CompileModule` runs the backend of a target and returns GLSL text or bytecode in one `CompiledShader`. With a
`ShaderCache` nothing is compiled twice: the key is a hash of the translated module, the target and
`kShaderCompilerVersion`. Because the module holds only the code a variant reaches, variants that end up with the same
code share one entry, and after the assembly is rebuilt only shaders whose reachable code changed are compiled again.
With a directory the cache keeps one file per shader (`<key>.hxsc`), written under a temporary name first; files that
are damaged or from another compiler version count as missing. A cache object is not thread safe.

## Reflection

```cpp
ShaderReflection vertex, fragment;
Reflect(vertexModule, vertex, error);
Reflect(fragmentModule, fragment, error);
PassReflection pass;
if (!LinkReflection(vertex, fragment, pass, error))
    Report(error);
const ReflectedMember* tint = pass.blocks[0].Find("tint");   // type, byte offset and size inside the block
```

`Reflect` describes one translated module, so one variant: its uniform blocks (slot, size, every member with type, byte
offset and size; members of nested structs as `light.color`), its textures, storage buffers and storage textures (slot,
writable, element type and size, format), the values entering and leaving the stage (vertex attributes are the
`Location` inputs of the vertex stage), the keywords it read and the work group size of a compute shader. Resources the
variant does not use are not listed. A texture is sampled with the sampler bound at its slot, so samplers are not listed.

`LinkReflection` joins the two stages of a pass and fails with a message naming the value when they do not fit: the
fragment stage reads a location the vertex stage does not write, or with another type or interpolation; a slot holds
different blocks or resources in the two stages; the vertex stage writes no position; two vertex inputs share a location.

## Material properties

```csharp
[Property("Tint"), Default(1.0f, 1.0f, 1.0f, 1.0f)] public float4 tint;
[Property("Base Map")] public Texture2D<float4> baseMap;

public struct Material { public float4 tint; public float4 baseMap_ST; }
[Uniform(1)] public static Material material;
[Texture(0)] public static Texture2D<float4> baseMap;     // in the pass class
```

```cpp
MaterialLayout layout;
BindProperties(shader, pass, layout, error);       // pass: the PassReflection of one variant
Array<u8> data = layout.blocks[0].defaults;        // what a new material uploads to slot layout.blocks[0].slot
WritePropertyValue(shader.properties[i], *layout.Find(i, PropertyTarget::Member), value, data);
```

A property reaches the code by its name: a number, vector or matrix property is the uniform block member of the same name,
a texture property the texture of the same name, and its tiling the float4 member `<name>_ST` (scale in xy, offset in zw)
unless the property is marked `[NoScaleOffset]`. `BindProperties` lists for every property where it goes (block and byte
offset, or texture slot), builds the default data of each block that takes properties (zero, the `[Default]` values
converted to the member type, tiling one and zero) and tells which blocks also hold members the renderer has to fill and
which properties the pass does not use. A property whose member or texture has another type is an error naming both.

## Bindless resources

`BindlessTexture` and `BindlessBuffer` are the indices the renderer registered a texture (with its sampler) or a buffer
under, so a shader reaches resources without binding slots: indices can sit in uniform blocks, buffers or vertex data.
In the compiled code they are plain `uint` values.

| C# | GLSL 4.50 (textures) / 4.30 (buffers) | Direct3D (shader model 5.0) |
|---|---|---|
| `texture.Sample(uv)`, `SampleLevel(uv, level)` | `texture(sampler2D(hx_textures[i]), uv)` with ARB_bindless_texture, handle table at binding 7 | entry from the table `t127` (pool, slice, sampler); a chain of conditions picks the array pool `t96 + pool` and the sampler `s8 + sampler`; fragment shaders sample with gradients taken before that flow |
| `buffer.Load(offset)` to `Load4` | heap words at binding 9, (offset, size) table at binding 8 | table `t125`, heap `t126` (`u7` in compute shaders) |
| `buffer.Store(offset, value)` to `Store4` | the same heap, writable | heap `u7`; compute shaders only |
| `buffer.Size` | size column of the table | size column of the table |

GLSL ES and GLSL 3.30 have no bindless resources; compiling such a shader for them is an error. `AsFloat`, `AsUInt` and
`AsInt` reinterpret bits (`uintBitsToFloat` and friends in GLSL, nothing at all in Direct3D).

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
| `[Storage(n, Format = f)] RWTexture2D<T> name` | `layout(f, binding = n) uniform image2D g_name` (`uimage2D`, `iimage2D`; `writeonly` when never read) | typed view `u<n>` |
| `image[texel] = value`, `image[texel]` | `imageStore`, `imageLoad` | `store_uav_typed`, `ld_uav_typed` |
| `InterlockedAdd(buffer, index, value)` | `atomicAdd` | `imm_atomic_iadd` |
| `[DispatchThreadId] uint3` | `gl_GlobalInvocationID` | `SV_DispatchThreadID` |

Direct3D buffers are read and written through raw (byte address) views, with the element size multiplied in by the
generated code. That way every buffer can be bound, also one that is a vertex buffer or holds indirect draw arguments,
which cannot be structured buffers. A storage texture is read only in the R32F, R32UI and R32I formats (Direct3D 11 reads no
other typed views), and its format follows T unless the attribute names one; `InterlockedAdd` works on buffers of `int` or
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

- `float`, `int`, `uint`, `bool`, `half`, the vector and matrix types of Helix.Shaders, structs of those (also nested). `half`
  values are floats with `mediump` declarations on GLSL ES; uniform blocks, buffers and vertex inputs reject them.
- Static methods, generic methods, `ref`, `out` and `in` parameters, ref locals, methods of user structs.
- Vector components and matrix columns by index (`v[i]`, `m[i]`), also written at computed positions.
- `if`, `for`, `while`, `do`, `switch`, `break`, `continue`, early `return`, `?:`, `&&`, `||`.
- The functions of `ShaderMath`, `Texture2D<T>`, `Texture2DArray<T>`, `Texture3D<T>` and `TextureCube<T>` with float, int or uint
  texels (`Sample` and `SampleLevel` for float texels, `Load` except on cubes, `Size`), `SampleCmp` and `SampleCmpLevelZero` of 2D,
  array and cube shadow maps with a `SamplerComparisonState`, and uniform structs, textures and samplers
  declared as static fields.
- `double` is computed as `float`.

Not available, each reported with the method name and the reason: classes and anything allocated, recursion, exceptions,
`goto`, 64 bit integers, calls into other libraries.
Dynamic keywords (decided at run time) are not available yet.

## Conventions of the generated GLSL

| C# | GLSL |
|---|---|
| vertex input `[Location(n)]` | `layout(location = n) in ... hx_in<n>` |
| value from vertex to fragment `[Location(n)]` | `hx_v<n>`, `flat` for integers |
| fragment output `[Target(n)]` | `layout(location = n) out ... hx_target<n>` |
| `[Position]` | `gl_Position` in the vertex stage, `gl_FragCoord` in the fragment stage |
| `[Uniform(slot)] static T name` | `layout(std140) uniform hx_block_name { T g_name; }` |
| `[Texture(slot)] static Texture2D<T> name` | `uniform sampler2D g_name` (`isampler2D`, `usampler2D` for integer texels, `sampler2DShadow` when read with a comparison sampler); the sampler state travels with the slot |

## Conventions of the generated Direct3D bytecode

| C# | Direct3D |
|---|---|
| vertex input `[Location(n)]` | `TEXCOORD<n>`, register n |
| value from vertex to fragment `[Location(n)]` | `TEXCOORD<n>`, register n + 1; integers and `Flat` are not interpolated |
| fragment output `[Target(n)]` | `SV_Target<n>` |
| `[Position]` | `SV_Position`, register 0 |
| `[VertexId]`, `[InstanceId]` | `SV_VertexID`, `SV_InstanceID` |
| `[FrontFacing]` (fragment input) | `SV_IsFrontFace`, after the locations |
| `[Depth]` (fragment output) | `SV_Depth` (`oDepth`) |
| `[Uniform(slot)] static T name` | constant buffer `b<slot>` |
| `[Texture(slot)] static Texture2D<T> name` | texture `t<slot>` with sampler `s<slot>`, return type from T |

A vector component or matrix column written at a computed index becomes a conditional move per element, since registers
cannot be indexed.
`asin`, `acos` and `atan` are polynomial approximations (error below 0.001), since the bytecode has no such instructions.

## Tests

- Assembly reader: tables, signatures, instructions and attributes of real assemblies, and 400 randomly damaged files.
- Execution: 17 C# shaders (arithmetic, branches, loops, `continue`, early returns from nested loops, switch, functions
  with `ref`/`out` and generics, library functions, integers, matrices, uniforms, textures, discard, bool vectors, nested
  structs) run on the CPU as C# and on the GPU as generated GLSL 3.30 and 4.50 and as Direct3D bytecode; the images must
  agree within 2/255. They run on the OpenGL device and on every Direct3D 11 adapter the machine offers.
- Direct3D container: chunks, sizes, signatures and registers of both stages, and that output is deterministic.
- GLSL ES 3.00 output is checked as text only; it is not executed yet.
- Compute: seven kernels (a uniform and a buffer, structs between two buffers on a 2D grid, an in place update with a
  helper that loops and returns early on a 3D grid, counters shared through atomic adds, a storage texture, bindless
  buffers, storage textures read and written in R32F, R32UI, R32I and written in RGBA16F) run on the CPU and are dispatched as GLSL 4.30 and as Direct3D
  bytecode on every adapter; the buffers must be the same byte by byte, texels within the last bit.
- Half precision: locals, a helper with half parameters and a half4 target; GLSL ES declarations are `mediump`, other
  targets are unchanged; half in a uniform block and in a vertex input is rejected.
- Shadow maps: comparison sampling of a filtered 2D map, a layer of an array and a cube face, on D32 float textures.
- Texture kinds: an eight layer array (sampled and loaded), a volume filtered in three dimensions, a cube read at texel centers
  of all six faces, and an integer array.
- Typed textures: float, float2, uint, uint2, int and int4 textures, sampled or loaded, in 32 bit formats on the GPU.
- Indexed writes: vector components and matrix columns written and read at positions computed per pixel.
- Stage values: a case reads `[FrontFacing]` (drawn once per winding into the two halves of the target) and a case writes
  `[Depth]` (a probe at depth 0.5 shows where it wrote more).
- Bindless: a fragment case with two bindless textures (different samplers) and a bindless buffer, and a kernel that reads one
  bindless buffer and writes another; they run on GL 4.5 with ARB_bindless_texture and on the Direct3D emulation of every
  adapter, and are skipped where the device has no bindless resources.
- Material: properties of every kind tied to members, tiling and texture slots with their default data, a block that mixes
  properties with other members, a shader whose properties the pass does not use, and a type mismatch.
- Reflection: block members with offsets (also std140 corner cases and nested structs), resources per variant, compute
  shaders, and three pairs of stages that do not fit.
- Variants and cache: the variants of passes with plain and shader feature keywords, wrong values and the limit; six
  variants compile four distinct shaders through the cache; a cache directory survives a restart, and a flipped byte
  or a cut off file is detected and replaced.
- Keywords: a case with a bool and an enum keyword runs in all six combinations on the CPU and the GPU; the text tests
  check that the code, the helper function and the texture of other variants are gone.
- Definitions: a shader using every declarative feature is read back field by field, a pass of it is compiled with
  keyword values taken from the description, and three broken definitions give their messages.
- Rejected shaders: recursion, a uniform struct whose layout differs between C# and std140, a missing stage attribute.

`HELIX_PRINT_SHADERS=<case name>` prints the generated fragment shader of one execution case and writes its bytecode to
`<case name>.dxbc` in the working directory.

## Shaders of the Helix.RHI tests

Helix.RHI sits below this library and cannot compile shaders in its own build, so its tests and its example use shaders
compiled here; GLES gets no source for bindless shaders, since it has no bindless resources.
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
