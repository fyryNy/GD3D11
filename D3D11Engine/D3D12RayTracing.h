#pragma once

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gd3d11rt {

// Independent of native Gothic pointers. Mesh IDs are scoped to Generation.
struct Vertex {
    float Position[3];
    float Normal[3];
    float TexCoord[2];
    uint32_t Color;
};

struct Material {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> Texture;
    float Color[4] = { 1, 1, 1, 1 };
    float AlphaCutoff = 0.5f;
    bool AlphaTest = false;
    bool DoubleSided = false;
};

struct Mesh {
    uint64_t Id = 0;
    uint64_t Revision = 0;
    std::vector<Vertex> Vertices;
    std::vector<uint32_t> Indices;
    Material Surface;
};

struct Instance {
    uint64_t MeshId = 0;
    // Row-major object-to-world transform, matching D3D12_RAYTRACING_INSTANCE_DESC.
    float Transform[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    uint32_t Color = 0xffffffffu;
};

struct Scene {
    uint64_t Generation = 0;
    std::vector<Mesh> Meshes;
    std::vector<Instance> Instances;
    bool Valid = true;
    std::string Status;
};

struct Frame {
    ID3D11Texture2D* Depth = nullptr;
    ID3D11Texture2D* Normals = nullptr;
    ID3D11Texture2D* Specular = nullptr;
    ID3D11Texture2D* Diffuse = nullptr;
    ID3D11ShaderResourceView* EnvironmentCube = nullptr;
    // Exactly the matrices used by the deferred shader's row-vector mul().
    float InverseProjection[16] = {};
    float InverseView[16] = {};
    float Camera[3] = {};
    float SunDirection[3] = { 0, 1, 0 };
    float SunColor[3] = { 1, 1, 1 };
    float SunStrength = 1;
    float AmbientColor[3] = { 0.15f, 0.15f, 0.15f };
    float MaxDistance = 50000;
    float RayBias = 0.5f;
    // 1 = full resolution, 2 = half resolution. No stochastic sampling/denoiser.
    uint32_t ResolutionDivisor = 2;
    bool Shadows = true;
    bool Reflections = true;
};

// Optional D3D12 sidecar; the D3D11 renderer and its swap chain stay native.
// Construction allocates no GPU resources. Failed initialization/rendering returns
// false, so the caller can use its existing shadows and environment reflections.
class D3D12RayTracing {
public:
    D3D12RayTracing();
    ~D3D12RayTracing();
    D3D12RayTracing( const D3D12RayTracing& ) = delete;
    D3D12RayTracing& operator=( const D3D12RayTracing& ) = delete;

    bool Initialize( ID3D11Device1* device, ID3D11DeviceContext1* context, IDXGIAdapter* adapter );
    bool Render( const Scene& scene, const Frame& frame );
    void ResetScene();
    const std::string& GetStatus() const;
    ID3D11ShaderResourceView* GetSunShadowSRV() const;
    ID3D11ShaderResourceView* GetReflectionSRV() const;

private:
    struct Impl;
    std::unique_ptr<Impl> Backend;
};

} // namespace gd3d11rt
