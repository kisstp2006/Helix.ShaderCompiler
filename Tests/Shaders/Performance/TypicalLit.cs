using System.Runtime.CompilerServices;
using HelixEngine.Shaders;
using static HelixEngine.Shaders.ShaderMath;

namespace Helix.ShaderTests;

public struct PointLight
{
    // xyz: position in world space, w: range.
    public float4 positionRange;
    public float4 color;
}

[InlineArray(4)]
public struct PointLights
{
    private PointLight element;
}

/// <summary>
/// A typical lit surface shader, the one the compile time target is measured on: world space lighting with a main
/// light and up to four point lights (Blinn-Phong), albedo, mask and normal maps, a filtered shadow map, fog and alpha
/// cutoff.
/// </summary>
[Shader("Tests/TypicalLit")]
public sealed class TypicalLit : ShaderDefinition
{
    [Property("Base Color")] public float4 baseColor;
    [Property("Smoothness"), Range(0.0f, 1.0f)] public float smoothness;
    [Property("Metallic"), Range(0.0f, 1.0f)] public float metallic;
    [Property("Normal Strength")] public float normalStrength;

    [Keyword(ShaderFeature = true)] public static readonly bool NormalMap = false;
    [Keyword] public static readonly bool Shadows = false;
    [Keyword(KeywordScope.Global)] public static readonly bool Fog = false;

    [Pass("Forward", LightMode = "ForwardBase")]
    public static class Forward
    {
        public struct PerCamera
        {
            public float4x4 viewProjection;
            public float4x4 worldToShadow;
            public float4 position;
            // xyz: color, w: density.
            public float4 fog;
        }

        public struct PerDraw
        {
            public float4x4 objectToWorld;
        }

        public struct PerFrame
        {
            public float4 mainDirection;
            public float4 mainColor;
            public float4 ambient;
            public PointLights points;
            public int pointCount;
            public float shadowTexel;
        }

        public struct Surface
        {
            public float4 baseColor;
            public float4 emission;
            public float4 uvScaleOffset;
            public float smoothness;
            public float metallic;
            public float normalStrength;
            public float alphaCutoff;
        }

        public struct Varyings
        {
            [Position] public float4 position;
            [Location(0)] public float3 worldPosition;
            [Location(1)] public float3 normal;
            [Location(2)] public float4 tangent;
            [Location(3)] public float2 uv;
            [Location(4)] public float4 shadowCoord;
            [Location(5)] public float fogFactor;
        }

        [Uniform(0)] public static PerCamera camera;
        [Uniform(1)] public static PerDraw draw;
        [Uniform(2)] public static PerFrame lighting;
        [Uniform(3)] public static Surface surface;
        [Texture(0)] public static Texture2D<float4> albedoMap;
        [Sampler(0)] public static SamplerState albedoSampler;
        [Texture(1)] public static Texture2D<float4> normalMap;
        [Sampler(1)] public static SamplerState normalSampler;
        [Texture(2)] public static Texture2D<float4> maskMap;
        [Sampler(2)] public static SamplerState maskSampler;
        [Texture(3)] public static Texture2D<float> shadowMap;
        [Sampler(3)] public static SamplerComparisonState shadowSampler;

        [VertexShader]
        public static Varyings Vertex([Location(0)] float3 position, [Location(1)] float3 normal, [Location(2)] float4 tangent, [Location(3)] float2 uv)
        {
            Varyings o;
            float4 world = Mul(draw.objectToWorld, new float4(position, 1.0f));
            o.position = Mul(camera.viewProjection, world);
            o.worldPosition = world.xyz;
            o.normal = Normalize(Mul(draw.objectToWorld, new float4(normal, 0.0f)).xyz);
            o.tangent = new float4(Normalize(Mul(draw.objectToWorld, new float4(tangent.xyz, 0.0f)).xyz), tangent.w);
            o.uv = uv * surface.uvScaleOffset.xy + surface.uvScaleOffset.zw;
            o.shadowCoord = Shadows ? Mul(camera.worldToShadow, world) : new float4(0.0f);
            o.fogFactor = Fog ? Saturate(Exp2(-camera.fog.w * o.position.z)) : 1.0f;
            return o;
        }

        private static float3 BlinnPhong(float3 n, float3 v, float3 l, float3 radiance, float3 diffuse, float3 specular, float shininess)
        {
            float lambert = Saturate(Dot(n, l));
            float3 h = Normalize(l + v);
            float highlight = Pow(Saturate(Dot(n, h)), shininess) * (shininess + 8.0f) * 0.0397887f;
            return (diffuse + specular * highlight) * radiance * lambert;
        }

        private static float SampleShadow(float4 coord)
        {
            float3 c = coord.xyz / coord.w;
            float sum = 0.0f;
            for (int k = 0; k < 4; k++)
            {
                float2 offset = new float2((k & 1) == 0 ? -0.5f : 0.5f, (k & 2) == 0 ? -0.5f : 0.5f) * lighting.shadowTexel;
                sum += shadowMap.SampleCmp(shadowSampler, c.xy + offset, c.z);
            }
            return sum * 0.25f;
        }

        [FragmentShader]
        [return: Target(0)]
        public static float4 Fragment(Varyings input)
        {
            float4 albedo = albedoMap.Sample(albedoSampler, input.uv) * surface.baseColor;
            if (albedo.w < surface.alphaCutoff)
                Discard();
            float4 mask = maskMap.Sample(maskSampler, input.uv);
            float metal = mask.x * surface.metallic;
            float occlusion = mask.y;
            float gloss = mask.w * surface.smoothness;

            float3 n = Normalize(input.normal);
            if (NormalMap)
            {
                float3 t = Normalize(input.tangent.xyz - n * Dot(n, input.tangent.xyz));
                float3 b = Cross(n, t) * input.tangent.w;
                float2 xy = (normalMap.Sample(normalSampler, input.uv).xy * 2.0f - new float2(1.0f)) * surface.normalStrength;
                float z = Sqrt(Saturate(1.0f - Dot(xy, xy)));
                n = Normalize(t * xy.x + b * xy.y + n * z);
            }

            float3 v = Normalize(camera.position.xyz - input.worldPosition);
            float3 diffuse = albedo.xyz * (1.0f - metal);
            float3 specular = Lerp(new float3(0.04f), albedo.xyz, metal);
            float shininess = Exp2(10.0f * gloss + 1.0f);
            float shadow = Shadows ? SampleShadow(input.shadowCoord) : 1.0f;

            float3 color = BlinnPhong(n, v, Normalize(-lighting.mainDirection.xyz), lighting.mainColor.xyz * shadow, diffuse, specular, shininess);
            for (int i = 0; i < lighting.pointCount; i++)
            {
                PointLight light = lighting.points[i];
                float3 d = light.positionRange.xyz - input.worldPosition;
                float distance = Length(d);
                float falloff = Saturate(1.0f - distance / light.positionRange.w);
                color += BlinnPhong(n, v, d / Max(distance, 0.0001f), light.color.xyz * (falloff * falloff), diffuse, specular, shininess);
            }
            color += lighting.ambient.xyz * diffuse * occlusion + surface.emission.xyz;
            if (Fog)
                color = Lerp(camera.fog.xyz, color, input.fogFactor);
            return new float4(color, albedo.w);
        }
    }
}
