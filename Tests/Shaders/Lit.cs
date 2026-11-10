using HelixEngine.Shaders;
using static HelixEngine.Shaders.ShaderMath;

namespace Helix.ShaderTests;

[Shader("Tests/Lit")]
public sealed class Lit : ShaderDefinition
{
    [Property("Base Color")] public float4 baseColor;
    [Property("Smoothness"), Range(0.0f, 1.0f)] public float smoothness;

    [Keyword(KeywordScope.Global, ShaderFeature = true)] public static readonly bool Fog = false;

    [Pass("Forward", LightMode = "ForwardBase", ZWrite = false, Cull = CullMode.Front)]
    public static class Forward
    {
        public struct Draw
        {
            public float4x4 modelViewProjection;
            public float4 tint;
        }

        public struct Varyings
        {
            [Position] public float4 position;
            [Location(0)] public float2 uv;
            [Location(1), Interpolation(InterpolationMode.Flat)] public uint material;
        }

        [Uniform(2)] public static Draw draw;
        [Texture(1)] public static Texture2D<float4> albedo;
        [Sampler(1)] public static SamplerState albedoSampler;
        [Storage(3)] public static RWStructuredBuffer<uint> counters;

        [VertexShader]
        public static Varyings Vertex([Location(0)] float3 position, [Location(1)] float2 uv, [InstanceId] uint instance)
        {
            Varyings o;
            o.position = Mul(draw.modelViewProjection, new float4(position, 1.0f));
            o.uv = uv;
            o.material = instance;
            return o;
        }

        [FragmentShader]
        [return: Target(0)]
        public static float4 Fragment(Varyings input)
        {
            float4 texel = albedo.Sample(albedoSampler, input.uv);
            float3 color = texel.xyz * draw.tint.xyz;
            if (Fog)
                color = Lerp(color, new float3(0.5f), Saturate(input.position.z));
            if (texel.w < 0.5f)
                Discard();
            return new float4(color, texel.w);
        }

        [ComputeShader(8, 4)]
        public static void Count([DispatchThreadId] uint3 id)
        {
            counters[(int)(id.x + id.y * 8u)] = Dot(new float2(id.x, id.y), new float2(1.0f)) > 4.0f ? 1u : 0u;
        }
    }
}
