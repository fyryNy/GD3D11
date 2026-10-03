// Optional DXR 1.1 pass. These rays traverse scene triangles, including geometry
// outside the current screen. This is not screen-space reflection ray marching.
#define MAX_RT_TEXTURES 2048
#define MAX_RT_MESHES 8192

struct Vertex {
    float3 Position;
    float3 Normal;
    float2 TexCoord;
    uint Color;
};
struct MeshData {
    float4 Color;
    uint TextureIndex;
    uint Flags;
    float AlphaCutoff;
    uint Padding;
};
struct InstanceData { uint MeshIndex; uint Color; uint2 Padding; };

cbuffer FrameConstants : register(b0) {
    row_major float4x4 InverseProjection;
    row_major float4x4 InverseView;
    float3 Camera; float RayBias;
    float3 SunDirection; float MaxDistance;
    float3 SunColor; float SunStrength;
    float3 AmbientColor; uint Flags;
    uint2 OutputSize; uint2 InputSize;
};

RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float4> Normals : register(t2);
Texture2D<float2> Specular : register(t3);
Texture2D<float4> Diffuse : register(t4);
StructuredBuffer<MeshData> Meshes : register(t5);
StructuredBuffer<InstanceData> Instances : register(t6);
TextureCube<float4> Environment : register(t7);
Texture2D<float4> MaterialTextures[MAX_RT_TEXTURES] : register(t8);
StructuredBuffer<Vertex> VertexBuffers[MAX_RT_MESHES] : register(t2056);
StructuredBuffer<uint> IndexBuffers[MAX_RT_MESHES] : register(t10248);
RWTexture2D<float> SunVisibility : register(u0);
RWTexture2D<float4> Reflection : register(u1);
SamplerState WrapSampler : register(s0);
SamplerState ClampSampler : register(s1);

float4 UnpackColor(uint color) {
    return float4((color >> 16) & 255, (color >> 8) & 255, color & 255, color >> 24) / 255.0f;
}

void TriangleVertices(uint meshIndex, uint primitive, out Vertex a, out Vertex b, out Vertex c) {
    uint descriptor = NonUniformResourceIndex(meshIndex);
    uint base = primitive * 3;
    a = VertexBuffers[descriptor][IndexBuffers[descriptor][base]];
    b = VertexBuffers[descriptor][IndexBuffers[descriptor][base + 1]];
    c = VertexBuffers[descriptor][IndexBuffers[descriptor][base + 2]];
}

bool AcceptTriangle(uint instanceIndex, uint primitive, float2 barycentrics, bool frontFace, bool shadowRay) {
    InstanceData instance = Instances[instanceIndex];
    MeshData mesh = Meshes[instance.MeshIndex];
    if (!shadowRay && !frontFace && (mesh.Flags & 2) == 0) return false;
    if ((mesh.Flags & 1) == 0) return true;
    Vertex a, b, c;
    TriangleVertices(instance.MeshIndex, primitive, a, b, c);
    float3 weights = float3(1 - barycentrics.x - barycentrics.y, barycentrics);
    float2 uv = a.TexCoord * weights.x + b.TexCoord * weights.y + c.TexCoord * weights.z;
    float alpha = MaterialTextures[NonUniformResourceIndex(mesh.TextureIndex)].SampleLevel(WrapSampler, uv, 0).a;
    // Native vertex/instance colors hold baked lighting, not transparency. The
    // raster material's alpha test also tests only the sampled texture alpha.
    return alpha >= mesh.AlphaCutoff;
}

float TraceSun(float3 position, float3 normal) {
    if (SunDirection.y <= 0 || dot(SunDirection, SunDirection) < 0.0001) return 0;
    RayDesc ray;
    ray.Origin = position + normal * RayBias;
    ray.Direction = normalize(SunDirection);
    ray.TMin = RayBias * 0.25;
    ray.TMax = MaxDistance;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(Scene, RAY_FLAG_NONE, 255, ray);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            AcceptTriangle(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(),
                query.CandidateTriangleBarycentrics(), query.CandidateTriangleFrontFace(), true))
            query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() == COMMITTED_NOTHING ? 1.0 : 0.0;
}

float4 TraceReflection(float3 position, float3 normal, float3 incident) {
    RayDesc ray;
    ray.Origin = position + normal * RayBias;
    ray.Direction = normalize(reflect(incident, normal));
    ray.TMin = RayBias * 0.25;
    ray.TMax = MaxDistance;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(Scene, RAY_FLAG_NONE, 255, ray);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            AcceptTriangle(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(),
                query.CandidateTriangleBarycentrics(), query.CandidateTriangleFrontFace(), false))
            query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() == COMMITTED_NOTHING) {
        if ((Flags & 4) == 0) return 0;
        float4 sky = Environment.SampleLevel(ClampSampler, ray.Direction, 0);
        return float4(max(sky.rgb, 0), saturate(sky.a));
    }
    InstanceData instance = Instances[query.CommittedInstanceID()];
    MeshData mesh = Meshes[instance.MeshIndex];
    Vertex a, b, c;
    TriangleVertices(instance.MeshIndex, query.CommittedPrimitiveIndex(), a, b, c);
    float2 bary = query.CommittedTriangleBarycentrics();
    float3 weights = float3(1 - bary.x - bary.y, bary);
    float2 uv = a.TexCoord * weights.x + b.TexCoord * weights.y + c.TexCoord * weights.z;
    float3 objectNormal = a.Normal * weights.x + b.Normal * weights.y + c.Normal * weights.z;
    if (dot(objectNormal, objectNormal) < 0.000001)
        objectNormal = cross(b.Position - a.Position, c.Position - a.Position);
    float3 worldNormal = normalize(mul(objectNormal, (float3x3)query.CommittedWorldToObject3x4()));
    if (dot(worldNormal, ray.Direction) > 0) worldNormal = -worldNormal;
    float3 hitPosition = ray.Origin + ray.Direction * query.CommittedRayT();
    float3 vertexColor = UnpackColor(a.Color).rgb * weights.x + UnpackColor(b.Color).rgb * weights.y + UnpackColor(c.Color).rgb * weights.z;
    float3 albedo = MaterialTextures[NonUniformResourceIndex(mesh.TextureIndex)].SampleLevel(WrapSampler, uv, 0).rgb;
    albedo *= mesh.Color.rgb;
    float bakedLighting = saturate(vertexColor.g * UnpackColor(instance.Color).g);
    float sunlight = SunStrength > 0 ? TraceSun(hitPosition, worldNormal) * saturate(dot(worldNormal, normalize(SunDirection))) : 0;
    return float4(max(albedo * (AmbientColor * bakedLighting + SunColor * SunStrength * sunlight), 0), 1);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatch : SV_DispatchThreadID) {
    uint2 pixel = dispatch.xy;
    if (any(pixel >= OutputSize)) return;
    float2 uv = (float2(pixel) + 0.5) / float2(OutputSize);
    uint2 inputPixel = min(uint2(uv * InputSize), InputSize - 1);
    float4 packedNormal = Normals.Load(int3(inputPixel, 0));
    SunVisibility[pixel] = 1;
    Reflection[pixel] = 0;
    if (packedNormal.a < 0.001 || dot(packedNormal.xyz, packedNormal.xyz) < 0.001) return;
    float depth = Depth.Load(int3(inputPixel, 0));
    float2 inputUV = (float2(inputPixel) + 0.5) / float2(InputSize);
    float4 viewPosition = mul(float4(inputUV * float2(2, -2) + float2(-1, 1), depth, 1), InverseProjection);
    if (abs(viewPosition.w) < 0.000001) return;
    viewPosition /= viewPosition.w;
    float3 position = mul(float4(viewPosition.xyz, 1), InverseView).xyz;
    float3 normal = normalize(mul(normalize(packedNormal.xyz), (float3x3)InverseView));
    if ((Flags & 1) != 0) SunVisibility[pixel] = TraceSun(position, normal);
    float2 spec = Specular.Load(int3(inputPixel, 0));
    if ((Flags & 2) != 0 && spec.x > 0.001 && spec.y >= 16)
        Reflection[pixel] = TraceReflection(position, normal, normalize(position - Camera));
}
