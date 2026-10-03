#include "pch.h"
#include "D3D12RayTracing.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace gd3d11rt {
using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT MaxTextures = 2048;
constexpr UINT MaxMeshes = 8192;
constexpr UINT SrvCount = 7 + MaxTextures + MaxMeshes * 2;
constexpr UINT DescriptorCount = SrvCount + 2;
constexpr UINT FrameCount = 3;
constexpr UINT64 GeometryBudget = 512ull * 1024 * 1024;
constexpr UINT64 TextureBudget = 256ull * 1024 * 1024;

struct Module {
    HMODULE Value = nullptr;
    ~Module() { if ( Value ) FreeLibrary( Value ); }
};

struct SharedTexture {
    ComPtr<ID3D12Resource> Resource;
    ComPtr<ID3D11Texture2D> Texture;
    ComPtr<ID3D11ShaderResourceView> SRV;
    UINT Width = 0, Height = 0;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
};

struct MeshData {
    float Color[4];
    UINT TextureIndex, Flags;
    float AlphaCutoff;
    UINT Padding;
};
struct InstanceData { UINT MeshIndex, Color, Padding[2]; };

struct alignas(16) FrameConstants {
    float InverseProjection[16], InverseView[16];
    float Camera[3], RayBias;
    float SunDirection[3], MaxDistance;
    float SunColor[3], SunStrength;
    float AmbientColor[3]; UINT Flags;
    UINT OutputSize[2], InputSize[2];
};
static_assert( sizeof( Vertex ) == 36, "DXIL vertex layout must match" );
static_assert( sizeof( MeshData ) == 32 && sizeof( InstanceData ) == 16, "DXIL metadata layout must match" );
static_assert( sizeof( FrameConstants ) == 208, "DXIL constant buffer layout must match" );

const char* BlitSource = R"(
struct Varyings { float4 Position : SV_POSITION; float2 UV : TEXCOORD; };
Varyings VSMain(uint id : SV_VertexID) {
    Varyings o;
    o.UV = float2((id << 1) & 2, id & 2);
    o.Position = float4(o.UV * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
Texture2D<float4> Source : register(t0);
TextureCube<float4> Cube : register(t1);
SamplerState LinearSampler : register(s0);
cbuffer BlitConstants : register(b0) { uint Face; uint3 Pad; };
float4 PSMain(Varyings input) : SV_TARGET { return Source.Sample(LinearSampler, input.UV); }
float4 PSCube(Varyings input) : SV_TARGET {
    float2 p = input.UV * 2 - 1;
    float3 direction;
    if (Face == 0) direction = float3(1, -p.y, -p.x);
    else if (Face == 1) direction = float3(-1, -p.y, p.x);
    else if (Face == 2) direction = float3(p.x, 1, p.y);
    else if (Face == 3) direction = float3(p.x, -1, -p.y);
    else if (Face == 4) direction = float3(p.x, -p.y, 1);
    else direction = float3(-p.x, -p.y, -1);
    return Cube.SampleLevel(LinearSampler, direction, 0);
}
)";

D3D12_HEAP_PROPERTIES HeapProperties( D3D12_HEAP_TYPE type ) {
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = type;
    properties.CreationNodeMask = properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC BufferDescription( UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE ) {
    D3D12_RESOURCE_DESC description = {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = std::max<UINT64>( size, 256 );
    description.Height = description.DepthOrArraySize = description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    return description;
}

D3D12_RESOURCE_DESC SharedTextureDescription( UINT width, UINT height, DXGI_FORMAT format,
    UINT arraySize, D3D12_RESOURCE_FLAGS flags ) {
    D3D12_RESOURCE_DESC description = {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = static_cast<UINT16>(arraySize);
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    // Render-target capability also makes VKD3D export a D3D11-compatible
    // resource descriptor. DXVK cannot import its native D3D12 descriptor
    // for SRV-only or UAV-only textures, even when their formats match.
    description.Flags = flags | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return description;
}

bool Finite( const float* values, size_t count ) {
    for ( size_t i = 0; i < count; ++i ) if ( !std::isfinite( values[i] ) ) return false;
    return true;
}

std::filesystem::path GameDirectory() {
    wchar_t path[32768];
    DWORD size = GetModuleFileNameW( nullptr, path, static_cast<DWORD>(std::size(path)) );
    if ( size == 0 || size >= std::size(path) ) return {};
    return std::filesystem::path( std::wstring( path, size ) ).parent_path();
}
} // namespace

struct D3D12RayTracing::Impl {
    // Modules outlive every COM object that may execute code from them.
    Module D3D12Module, DxilModule, DxcModule;
    ComPtr<ID3D11Device1> Device11;
    ComPtr<ID3D11Device5> Device11Fences;
    ComPtr<ID3D11DeviceContext1> Context11;
    ComPtr<ID3D11DeviceContext4> Context11Fences;
    ComPtr<ID3D12Device5> Device;
    ComPtr<ID3D12CommandQueue> Queue;
    ComPtr<ID3D12Fence> InputFence, CompletionFence;
    ComPtr<ID3D11Fence> InputFence11, CompletionFence11;
    ComPtr<ID3D12DescriptorHeap> Descriptors;
    ComPtr<ID3D12RootSignature> RootSignature;
    ComPtr<ID3D12PipelineState> Pipeline;
    ComPtr<ID3D11VertexShader> BlitVS;
    ComPtr<ID3D11PixelShader> BlitPS, BlitCubePS;
    ComPtr<ID3D11SamplerState> BlitSampler;
    ComPtr<ID3D11Buffer> BlitConstants;
    ComPtr<ID3D11RasterizerState> BlitRasterizer;
    ComPtr<ID3D11DepthStencilState> BlitDepth;
    ComPtr<ID3DDeviceContextState> BlitState;
    struct Slot {
        ComPtr<ID3D12CommandAllocator> Allocator;
        ComPtr<ID3D12GraphicsCommandList4> List;
        ComPtr<ID3D12DescriptorHeap> Descriptors;
        std::vector<ComPtr<ID3D12Resource>> Lifetime;
        UINT64 Completion = 0;
        bool DescriptorsInitialized = false;
    };
    std::array<Slot, FrameCount> Slots;
    struct GpuMesh {
        UINT Index = 0;
        UINT64 Revision = 0, Bytes = 0;
        size_t VertexCount = 0, IndexCount = 0;
        ComPtr<ID3D12Resource> Vertices, Indices, Acceleration;
    };
    struct CachedTexture {
        ComPtr<ID3D11ShaderResourceView> Source;
        SharedTexture Copy;
    };
    std::unordered_map<uint64_t, GpuMesh> Meshes;
    std::unordered_map<ID3D11ShaderResourceView*, UINT> TextureIndices;
    std::vector<CachedTexture> Textures;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> LastInstances;
    SharedTexture WhiteTexture, Environment;
    ComPtr<ID3D11ShaderResourceView> EnvironmentSource;
    std::array<SharedTexture, 4> Inputs;
    SharedTexture SunOutput, ReflectionOutput;
    ComPtr<ID3D12Resource> TopLevel, TopLevelScratch;
    UINT64 TopLevelSize = 0, TopLevelScratchSize = 0;
    UINT DescriptorStride = 0, NextMesh = 0, NextSlot = 0;
    UINT64 InputValue = 0, CompletionValue = 0, Generation = 0, GeometryBytes = 0, TextureBytes = 0;
    HANDLE CompletionEvent = nullptr;
    bool Initialized = false, AttemptedInitialization = false, ValidOutput = false, HaveGeneration = false;
    std::string Status = "Ray tracing has not been initialized.";
    using SerializeSignature = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    SerializeSignature SerializeRoot = nullptr;

    ~Impl() {
        // Optional rendering must also be destructible while the caller handles
        // a failed allocation. Fence error formatting can allocate a string.
        try { WaitIdle(); } catch (...) {}
        if ( CompletionEvent ) CloseHandle( CompletionEvent );
    }

    bool Error( const char* operation, HRESULT hr = E_FAIL ) {
        std::ostringstream message;
        message << operation << " (0x" << std::hex << static_cast<uint32_t>(hr) << "). Using the existing renderer.";
        Status = message.str();
        ValidOutput = false;
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CPU( UINT index ) const {
        auto handle = Descriptors->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * DescriptorStride;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE GPU( const Slot& slot, UINT index ) const {
        auto handle = slot.Descriptors->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * DescriptorStride;
        return handle;
    }

    bool WaitFor( UINT64 value ) {
        if ( !CompletionFence || value == 0 ) return true;
        const UINT64 complete = CompletionFence->GetCompletedValue();
        if ( complete == UINT64_MAX ) return Error( "The ray tracing device was removed", Device->GetDeviceRemovedReason() );
        if ( complete >= value ) return true;
        HRESULT hr = CompletionFence->SetEventOnCompletion( value, CompletionEvent );
        if ( FAILED( hr ) ) return Error( "Could not wait for ray tracing resources", hr );
        if ( WaitForSingleObject( CompletionEvent, 10000 ) != WAIT_OBJECT_0 )
            return Error( "Timed out waiting for ray tracing resources", DXGI_ERROR_DEVICE_HUNG );
        return true;
    }

    bool WaitIdle() {
        if ( !Queue || !CompletionFence || !CompletionEvent ) return true;
        const UINT64 value = ++CompletionValue;
        HRESULT hr = Queue->Signal( CompletionFence.Get(), value );
        return SUCCEEDED( hr ) ? WaitFor( value ) : Error( "Could not finish ray tracing work", hr );
    }

    bool Buffer( UINT64 size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state,
        ComPtr<ID3D12Resource>& resource, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE ) {
        const auto heap = HeapProperties( heapType );
        const auto description = BufferDescription( size, flags );
        HRESULT hr = Device->CreateCommittedResource( &heap, D3D12_HEAP_FLAG_NONE, &description, state,
            nullptr, IID_PPV_ARGS(resource.ReleaseAndGetAddressOf()) );
        return SUCCEEDED( hr ) || Error( "Could not allocate ray tracing geometry", hr );
    }

    bool Upload( Slot& slot, const void* bytes, UINT64 size, ComPtr<ID3D12Resource>& resource ) {
        if ( size == 0 || size > std::numeric_limits<size_t>::max() ) return Error( "Invalid ray tracing upload size", E_INVALIDARG );
        if ( !Buffer( size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, resource ) ) return false;
        void* mapping = nullptr;
        const D3D12_RANGE noRead = { 0, 0 };
        HRESULT hr = resource->Map( 0, &noRead, &mapping );
        if ( FAILED( hr ) ) return Error( "Could not upload ray tracing data", hr );
        memcpy( mapping, bytes, static_cast<size_t>(size) );
        const D3D12_RANGE written = { 0, static_cast<SIZE_T>(size) };
        resource->Unmap( 0, &written );
        slot.Lifetime.push_back( resource );
        return true;
    }

    void Transition( ID3D12GraphicsCommandList4* list, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after ) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier( 1, &barrier );
    }

    void UAVBarrier( ID3D12GraphicsCommandList4* list, ID3D12Resource* resource ) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        list->ResourceBarrier( 1, &barrier );
    }

    bool Shared( UINT width, UINT height, DXGI_FORMAT format, UINT arraySize,
        D3D12_RESOURCE_FLAGS flags, SharedTexture& texture ) {
        const auto description = SharedTextureDescription( width, height, format, arraySize, flags );
        const auto heap = HeapProperties( D3D12_HEAP_TYPE_DEFAULT );
        SharedTexture created;
        HRESULT hr = Device->CreateCommittedResource( &heap, D3D12_HEAP_FLAG_SHARED, &description,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(created.Resource.GetAddressOf()) );
        if ( FAILED( hr ) ) return Error( "Could not share a ray tracing texture", hr );
        HANDLE sharedHandle = nullptr;
        hr = Device->CreateSharedHandle( created.Resource.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle );
        if ( FAILED( hr ) ) return Error( "Could not export a ray tracing texture", hr );
        hr = Device11->OpenSharedResource1( sharedHandle, IID_PPV_ARGS(created.Texture.GetAddressOf()) );
        CloseHandle( sharedHandle );
        if ( FAILED( hr ) ) return Error( "Could not import a ray tracing texture into Direct3D 11", hr );
        D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = format;
        if ( arraySize > 1 ) {
            // The D3D12 import has array metadata, but no D3D11 TEXTURECUBE
            // creation flag. Its DX11 view is only used for conversion targets.
            srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.MipLevels = 1;
            srv.Texture2DArray.ArraySize = arraySize;
        } else {
            srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
        }
        hr = Device11->CreateShaderResourceView( created.Texture.Get(), &srv, created.SRV.GetAddressOf() );
        if ( FAILED( hr ) ) return Error( "Could not view a shared ray tracing texture", hr );
        created.Width = width; created.Height = height; created.Format = format;
        texture = std::move( created );
        return true;
    }

    void TextureDescriptor( const SharedTexture& texture, UINT index, bool cube = false ) {
        D3D12_SHADER_RESOURCE_VIEW_DESC description = {};
        description.Format = texture.Format;
        description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if ( cube ) {
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            description.TextureCube.MipLevels = 1;
        } else {
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            description.Texture2D.MipLevels = 1;
        }
        Device->CreateShaderResourceView( texture.Resource.Get(), &description, CPU(index) );
    }

    void BufferDescriptor( ID3D12Resource* resource, UINT count, UINT stride, UINT index ) {
        D3D12_SHADER_RESOURCE_VIEW_DESC description = {};
        description.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        description.Buffer.NumElements = count;
        description.Buffer.StructureByteStride = stride;
        Device->CreateShaderResourceView( resource, &description, CPU(index) );
    }

    bool CreateBlitter() {
        ComPtr<ID3DBlob> vs, ps, cube, errors;
        HRESULT hr = D3DCompile( BlitSource, strlen(BlitSource), "RayTracingTextureBlit", nullptr, nullptr,
            "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vs.GetAddressOf(), errors.GetAddressOf() );
        if ( SUCCEEDED( hr ) ) hr = D3DCompile( BlitSource, strlen(BlitSource), "RayTracingTextureBlit", nullptr, nullptr,
            "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, ps.GetAddressOf(), errors.ReleaseAndGetAddressOf() );
        if ( SUCCEEDED( hr ) ) hr = D3DCompile( BlitSource, strlen(BlitSource), "RayTracingTextureBlit", nullptr, nullptr,
            "PSCube", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, cube.GetAddressOf(), errors.ReleaseAndGetAddressOf() );
        if ( FAILED( hr ) ) return Error( "Could not compile ray tracing texture conversion", hr );
        hr = Device11->CreateVertexShader( vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, BlitVS.GetAddressOf() );
        if ( SUCCEEDED( hr ) ) hr = Device11->CreatePixelShader( ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, BlitPS.GetAddressOf() );
        if ( SUCCEEDED( hr ) ) hr = Device11->CreatePixelShader( cube->GetBufferPointer(), cube->GetBufferSize(), nullptr, BlitCubePS.GetAddressOf() );
        if ( FAILED( hr ) ) return Error( "Could not create ray tracing texture conversion shaders", hr );
        D3D11_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        hr = Device11->CreateSamplerState( &sampler, BlitSampler.GetAddressOf() );
        D3D11_BUFFER_DESC constants = {};
        constants.ByteWidth = 16; constants.Usage = D3D11_USAGE_DEFAULT; constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if ( SUCCEEDED( hr ) ) hr = Device11->CreateBuffer( &constants, nullptr, BlitConstants.GetAddressOf() );
        D3D11_RASTERIZER_DESC rasterizer = {};
        rasterizer.FillMode = D3D11_FILL_SOLID; rasterizer.CullMode = D3D11_CULL_NONE; rasterizer.DepthClipEnable = TRUE;
        if ( SUCCEEDED( hr ) ) hr = Device11->CreateRasterizerState( &rasterizer, BlitRasterizer.GetAddressOf() );
        D3D11_DEPTH_STENCIL_DESC depth = {};
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if ( SUCCEEDED( hr ) ) hr = Device11->CreateDepthStencilState( &depth, BlitDepth.GetAddressOf() );
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if ( SUCCEEDED( hr ) ) hr = Device11->CreateDeviceContextState( 0, &level, 1, D3D11_SDK_VERSION,
            __uuidof(ID3D11Device), nullptr, BlitState.GetAddressOf() );
        return SUCCEEDED( hr ) || Error( "Could not create ray tracing texture conversion state", hr );
    }

    bool Blit( ID3D11ShaderResourceView* source, SharedTexture& target, bool cube = false ) {
        ComPtr<ID3DDeviceContextState> previous;
        Context11->SwapDeviceContextState( BlitState.Get(), previous.GetAddressOf() );
        struct Restore {
            ID3D11DeviceContext1* Context;
            ID3DDeviceContextState* State;
            ~Restore() { Context->SwapDeviceContextState( State, nullptr ); }
        } restore = { Context11.Get(), previous.Get() };
        Context11->IASetInputLayout( nullptr );
        Context11->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
        Context11->VSSetShader( BlitVS.Get(), nullptr, 0 );
        Context11->PSSetShader( cube ? BlitCubePS.Get() : BlitPS.Get(), nullptr, 0 );
        Context11->GSSetShader( nullptr, nullptr, 0 );
        Context11->HSSetShader( nullptr, nullptr, 0 );
        Context11->DSSetShader( nullptr, nullptr, 0 );
        Context11->RSSetState( BlitRasterizer.Get() );
        Context11->OMSetDepthStencilState( BlitDepth.Get(), 0 );
        Context11->OMSetBlendState( nullptr, nullptr, UINT_MAX );
        Context11->PSSetShaderResources( cube ? 1 : 0, 1, &source );
        Context11->PSSetSamplers( 0, 1, BlitSampler.GetAddressOf() );
        D3D11_VIEWPORT viewport = { 0, 0, static_cast<float>(target.Width), static_cast<float>(target.Height), 0, 1 };
        Context11->RSSetViewports( 1, &viewport );
        for ( UINT face = 0; face < (cube ? 6u : 1u); ++face ) {
            ComPtr<ID3D11RenderTargetView> rtv;
            D3D11_RENDER_TARGET_VIEW_DESC description = {};
            description.Format = target.Format;
            if ( cube ) {
                description.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                description.Texture2DArray.FirstArraySlice = face;
                description.Texture2DArray.ArraySize = 1;
            } else description.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            HRESULT hr = Device11->CreateRenderTargetView( target.Texture.Get(), &description, rtv.GetAddressOf() );
            if ( FAILED( hr ) ) return Error( "Could not convert a ray tracing material texture", hr );
            Context11->OMSetRenderTargets( 1, rtv.GetAddressOf(), nullptr );
            const UINT constants[4] = { face, 0, 0, 0 };
            Context11->UpdateSubresource( BlitConstants.Get(), 0, nullptr, constants, 0, 0 );
            Context11->PSSetConstantBuffers( 0, 1, BlitConstants.GetAddressOf() );
            if ( source ) Context11->Draw( 3, 0 );
            else {
                const float white[4] = { 1, 1, 1, 1 };
                Context11->ClearRenderTargetView( rtv.Get(), white );
            }
        }
        return true;
    }

    bool MaterialTexture( ID3D11ShaderResourceView* source, UINT& index ) {
        if ( !source ) { index = 0; return true; }
        auto existing = TextureIndices.find( source );
        if ( existing != TextureIndices.end() ) { index = existing->second; return true; }
        if ( Textures.size() + 1 >= MaxTextures ) return Error( "The ray tracing material texture limit was exceeded", E_OUTOFMEMORY );
        D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
        source->GetDesc( &srv );
        if ( srv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ) return Error( "Unsupported ray tracing material texture view", E_INVALIDARG );
        ComPtr<ID3D11Resource> resource;
        ComPtr<ID3D11Texture2D> texture;
        source->GetResource( resource.GetAddressOf() );
        HRESULT hr = resource.As( &texture );
        if ( FAILED( hr ) ) return Error( "Invalid ray tracing material texture", hr );
        D3D11_TEXTURE2D_DESC description = {};
        texture->GetDesc( &description );
        UINT width = std::max( 1u, description.Width >> srv.Texture2D.MostDetailedMip );
        UINT height = std::max( 1u, description.Height >> srv.Texture2D.MostDetailedMip );
        // Reserve an equal maximum allocation for every available material
        // descriptor, so HD textures cannot exhaust the cache before the rest
        // of a large world is uploaded. The original raster texture is intact.
        constexpr UINT64 perTextureBudget = TextureBudget / (MaxTextures - 1);
        auto reduceSize = [&] {
            width = std::max( 1u, width / 2 );
            height = std::max( 1u, height / 2 );
        };
        while ( std::max( width, height ) > 256 ) reduceSize();
        UINT64 bytes = 0;
        for ( ;; ) {
            const auto copyDescription = SharedTextureDescription( width, height,
                DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET );
            const auto allocation = Device->GetResourceAllocationInfo( 0, 1, &copyDescription );
            if ( allocation.SizeInBytes == std::numeric_limits<UINT64>::max() )
                return Error( "Invalid ray tracing material texture allocation", E_INVALIDARG );
            bytes = allocation.SizeInBytes;
            if ( bytes <= perTextureBudget ) break;
            if ( width == 1 && height == 1 )
                return Error( "The minimum ray tracing texture allocation exceeds its budget", E_OUTOFMEMORY );
            reduceSize();
        }
        if ( bytes > TextureBudget - TextureBytes ) return Error("The ray tracing material texture budget was exceeded", E_OUTOFMEMORY);
        CachedTexture cached;
        cached.Source = source;
        if ( !Shared( width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, cached.Copy ) ||
            !Blit( source, cached.Copy ) ) return false;
        index = static_cast<UINT>(Textures.size()) + 1;
        TextureDescriptor( cached.Copy, 7 + index );
        Textures.push_back( std::move(cached) );
        TextureIndices.emplace( source, index );
        TextureBytes += bytes;
        return true;
    }

    bool SharedFence( ComPtr<ID3D12Fence>& fence, ComPtr<ID3D11Fence>& fence11 ) {
        HRESULT hr = Device->CreateFence( 0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence.GetAddressOf()) );
        if ( FAILED( hr ) ) return Error( "Could not create a ray tracing synchronization fence", hr );
        HANDLE handle = nullptr;
        hr = Device->CreateSharedHandle( fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle );
        if ( FAILED( hr ) ) return Error( "Could not export a ray tracing synchronization fence", hr );
        hr = Device11Fences->OpenSharedFence( handle, IID_PPV_ARGS(fence11.GetAddressOf()) );
        CloseHandle( handle );
        return SUCCEEDED( hr ) || Error( "Could not synchronize Direct3D 11 and ray tracing", hr );
    }

    bool CompilePipeline() {
        const auto directory = GameDirectory();
        const auto shader = directory / L"GD3D11" / L"shaders" / L"CS_RayTracing.hlsl";
        using CreateDxc = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
        auto create = reinterpret_cast<CreateDxc>( GetProcAddress( DxcModule.Value, "DxcCreateInstance" ) );
        if ( !create ) return Error( "The DXIL compiler is unavailable", E_NOINTERFACE );
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        HRESULT hr = create( CLSID_DxcUtils, IID_PPV_ARGS(utils.GetAddressOf()) );
        if ( SUCCEEDED( hr ) ) hr = create( CLSID_DxcCompiler, IID_PPV_ARGS(compiler.GetAddressOf()) );
        if ( FAILED( hr ) ) return Error( "Could not initialize the DXIL compiler", hr );
        ComPtr<IDxcBlobEncoding> source;
        hr = utils->LoadFile( shader.c_str(), nullptr, source.GetAddressOf() );
        if ( FAILED( hr ) ) return Error( "Could not load GD3D11/shaders/CS_RayTracing.hlsl", hr );
        ComPtr<IDxcIncludeHandler> includes;
        hr = utils->CreateDefaultIncludeHandler( includes.GetAddressOf() );
        if ( FAILED( hr ) ) return Error( "Could not initialize DXIL shader includes", hr );
        DxcBuffer buffer = { source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_UTF8 };
        const wchar_t* arguments[] = { L"-E", L"CSMain", L"-T", L"cs_6_5", L"-O3", L"-HV", L"2021", L"-Qstrip_debug", L"-Qstrip_reflect" };
        ComPtr<IDxcResult> result;
        hr = compiler->Compile( &buffer, arguments, static_cast<UINT32>(std::size(arguments)), includes.Get(), IID_PPV_ARGS(result.GetAddressOf()) );
        if ( FAILED( hr ) ) return Error( "Could not compile the DXR shader", hr );
        HRESULT compilation = E_FAIL;
        hr = result->GetStatus( &compilation );
        if ( FAILED( hr ) || FAILED( compilation ) ) {
            ComPtr<IDxcBlobUtf8> errors;
            result->GetOutput( DXC_OUT_ERRORS, IID_PPV_ARGS(errors.GetAddressOf()), nullptr );
            Error( "Could not compile the DXR shader", FAILED(hr) ? hr : compilation );
            if ( errors && errors->GetStringLength() ) Status += " " + std::string(errors->GetStringPointer(), errors->GetStringLength());
            return false;
        }
        ComPtr<IDxcBlob> binary;
        hr = result->GetOutput( DXC_OUT_OBJECT, IID_PPV_ARGS(binary.GetAddressOf()), nullptr );
        if ( FAILED( hr ) ) return Error( "The DXIL compiler produced no ray tracing shader", hr );
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = SrvCount;
        ranges[0].BaseShaderRegister = 1;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        D3D12_ROOT_PARAMETER parameters[4] = {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[1].Descriptor.ShaderRegister = 0;
        parameters[2].ParameterType = parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[2].DescriptorTable.NumDescriptorRanges = parameters[3].DescriptorTable.NumDescriptorRanges = 1;
        parameters[2].DescriptorTable.pDescriptorRanges = &ranges[0];
        parameters[3].DescriptorTable.pDescriptorRanges = &ranges[1];
        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        for ( UINT i = 0; i < 2; ++i ) {
            samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = i ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samplers[i].ShaderRegister = i;
            samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        }
        D3D12_ROOT_SIGNATURE_DESC description = {};
        description.NumParameters = static_cast<UINT>(std::size(parameters)); description.pParameters = parameters;
        description.NumStaticSamplers = static_cast<UINT>(std::size(samplers)); description.pStaticSamplers = samplers;
        ComPtr<ID3DBlob> signature, errors;
        hr = SerializeRoot( &description, D3D_ROOT_SIGNATURE_VERSION_1, signature.GetAddressOf(), errors.GetAddressOf() );
        if ( FAILED( hr ) ) return Error( "Could not describe the ray tracing shader resources", hr );
        hr = Device->CreateRootSignature( 0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(RootSignature.GetAddressOf()) );
        if ( FAILED( hr ) ) return Error( "Could not create ray tracing shader resources", hr );
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
        pipeline.pRootSignature = RootSignature.Get();
        pipeline.CS = { binary->GetBufferPointer(), binary->GetBufferSize() };
        hr = Device->CreateComputePipelineState( &pipeline, IID_PPV_ARGS(Pipeline.GetAddressOf()) );
        return SUCCEEDED( hr ) || Error( "Could not create the DXR compute pipeline", hr );
    }

    bool Initialize( ID3D11Device1* device11, ID3D11DeviceContext1* context11, IDXGIAdapter* adapter ) {
        if ( Initialized ) return true;
        if ( AttemptedInitialization ) return false;
        AttemptedInitialization = true;
        if ( !device11 || !context11 || !adapter ) return Error( "Missing Direct3D 11 ray tracing inputs", E_INVALIDARG );
        Device11 = device11; Context11 = context11;
        HRESULT hr = Device11.As( &Device11Fences );
        if ( SUCCEEDED( hr ) ) hr = Context11.As( &Context11Fences );
        if ( FAILED( hr ) ) return Error( "Direct3D 11/12 shared fences are unsupported", hr );
        D3D12Module.Value = LoadLibraryExW( L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 );
        if ( !D3D12Module.Value ) return Error( "Direct3D 12 is unavailable", HRESULT_FROM_WIN32(GetLastError()) );
        using CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
        auto create = reinterpret_cast<CreateDevice>( GetProcAddress( D3D12Module.Value, "D3D12CreateDevice" ) );
        SerializeRoot = reinterpret_cast<SerializeSignature>( GetProcAddress( D3D12Module.Value, "D3D12SerializeRootSignature" ) );
        if ( !create || !SerializeRoot ) return Error( "Direct3D 12 is unavailable", E_NOINTERFACE );
        // Separate an incompatible DXGI/D3D12 runtime from a runtime that can
        // create this adapter's device but lacks the ray tracing interface.
        ComPtr<ID3D12Device> baseDevice;
        hr = create( adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(baseDevice.GetAddressOf()) );
        if ( FAILED( hr ) ) return Error( "Could not create a Direct3D 12 device on the renderer's GPU", hr );
        hr = baseDevice.As( &Device );
        if ( FAILED( hr ) ) return Error( "The Direct3D 12 runtime does not expose ray tracing device interfaces", hr );
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options = {};
        hr = Device->CheckFeatureSupport( D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options) );
        if ( FAILED( hr ) || options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1 )
            return Error( "This GPU/driver does not support DXR 1.1", FAILED(hr) ? hr : DXGI_ERROR_UNSUPPORTED );
        D3D12_FEATURE_DATA_SHADER_MODEL model = { D3D_SHADER_MODEL_6_5 };
        hr = Device->CheckFeatureSupport( D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model) );
        if ( FAILED( hr ) || model.HighestShaderModel < D3D_SHADER_MODEL_6_5 )
            return Error( "The ray tracing shader model is unsupported", FAILED(hr) ? hr : DXGI_ERROR_UNSUPPORTED );
        D3D12_FEATURE_DATA_D3D12_OPTIONS resources = {};
        hr = Device->CheckFeatureSupport( D3D12_FEATURE_D3D12_OPTIONS, &resources, sizeof(resources) );
        if ( FAILED( hr ) || resources.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3 )
            return Error( "The ray tracing texture binding tier is unsupported", FAILED(hr) ? hr : DXGI_ERROR_UNSUPPORTED );
        D3D12_COMMAND_QUEUE_DESC queue = {};
        queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = Device->CreateCommandQueue( &queue, IID_PPV_ARGS(Queue.GetAddressOf()) );
        if ( FAILED( hr ) ) return Error( "Could not create the ray tracing queue", hr );
        CompletionEvent = CreateEventW( nullptr, FALSE, FALSE, nullptr );
        if ( !CompletionEvent ) return Error( "Could not create the ray tracing completion event", HRESULT_FROM_WIN32(GetLastError()) );
        if ( !SharedFence(InputFence, InputFence11) || !SharedFence(CompletionFence, CompletionFence11) ) return false;
        D3D12_DESCRIPTOR_HEAP_DESC descriptors = {};
        descriptors.NumDescriptors = DescriptorCount;
        descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = Device->CreateDescriptorHeap( &descriptors, IID_PPV_ARGS(Descriptors.GetAddressOf()) );
        if ( FAILED( hr ) ) return Error( "Could not allocate ray tracing texture bindings", hr );
        DescriptorStride = Device->GetDescriptorHandleIncrementSize( descriptors.Type );
        for ( auto& slot : Slots ) {
            D3D12_DESCRIPTOR_HEAP_DESC frameDescriptors = descriptors;
            frameDescriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = Device->CreateDescriptorHeap( &frameDescriptors, IID_PPV_ARGS(slot.Descriptors.GetAddressOf()) );
            if ( FAILED(hr) ) return Error("Could not allocate per-frame ray tracing texture bindings", hr);
            hr = Device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.Allocator.GetAddressOf()) );
            if ( SUCCEEDED( hr ) ) hr = Device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.Allocator.Get(), nullptr, IID_PPV_ARGS(slot.List.GetAddressOf()) );
            if ( SUCCEEDED( hr ) ) hr = slot.List->Close();
            if ( FAILED( hr ) ) return Error( "Could not create ray tracing commands", hr );
        }
        const auto compilerPath = GameDirectory() / L"GD3D11" / L"Bin" / L"dxcompiler.dll";
        const auto validatorPath = compilerPath.parent_path() / L"dxil.dll";
        DxilModule.Value = LoadLibraryExW( validatorPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS );
        if ( !DxilModule.Value ) return Error( "GD3D11/Bin/dxil.dll is unavailable (the x86 DXIL validator is required)", HRESULT_FROM_WIN32(GetLastError()) );
        DxcModule.Value = LoadLibraryExW( compilerPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS );
        if ( !DxcModule.Value ) return Error( "GD3D11/Bin/dxcompiler.dll is unavailable (the x86 DXIL compiler is required)", HRESULT_FROM_WIN32(GetLastError()) );
        if ( !CompilePipeline() || !CreateBlitter() ) return false;
        if ( !Shared( 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, WhiteTexture ) ||
            !Blit( nullptr, WhiteTexture ) ) return false;
        TextureDescriptor( WhiteTexture, 7 );
        // Initialize unused descriptors too, so debug validation never observes
        // uninitialized entries in the large dynamically indexed tables.
        for ( UINT i = 1; i < MaxTextures; ++i ) TextureDescriptor( WhiteTexture, 7 + i );
        for ( UINT i = 0; i < MaxMeshes; ++i ) {
            BufferDescriptor( nullptr, 1, sizeof(Vertex), 7 + MaxTextures + i );
            BufferDescriptor( nullptr, 1, sizeof(UINT), 7 + MaxTextures + MaxMeshes + i );
        }
        Initialized = true;
        Status = "DXR 1.1 is available. Ray tracing uses the supplied scene geometry.";
        return true;
    }

    bool ResetScene() {
        ValidOutput = false;
        if ( !WaitIdle() ) return false;
        Meshes.clear(); TextureIndices.clear(); Textures.clear();
        LastInstances.clear();
        NextMesh = 0; GeometryBytes = TextureBytes = 0; HaveGeneration = false;
        TopLevel.Reset(); TopLevelScratch.Reset(); TopLevelSize = TopLevelScratchSize = 0;
        for ( auto& slot : Slots ) slot.Lifetime.clear();
        if ( Descriptors && WhiteTexture.Resource ) {
            for ( UINT i = 1; i < MaxTextures; ++i ) TextureDescriptor(WhiteTexture, 7 + i);
            for ( UINT i = 0; i < MaxMeshes; ++i ) {
                BufferDescriptor(nullptr, 1, sizeof(Vertex), 7 + MaxTextures + i);
                BufferDescriptor(nullptr, 1, sizeof(UINT), 7 + MaxTextures + MaxMeshes + i);
            }
            for ( auto& slot : Slots ) slot.DescriptorsInitialized = false;
        }
        return true;
    }

    bool BuildMesh( const Mesh& source, GpuMesh& mesh, Slot& slot ) {
        if ( source.Vertices.empty() || source.Indices.empty() || source.Indices.size() % 3 ||
            source.Vertices.size() > UINT_MAX || source.Indices.size() > UINT_MAX )
            return Error( "Invalid ray tracing mesh", E_INVALIDARG );
        for ( const auto& vertex : source.Vertices ) {
            if ( !Finite(vertex.Position, 3) || !Finite(vertex.Normal, 3) || !Finite(vertex.TexCoord, 2) )
                return Error( "Non-finite ray tracing geometry", E_INVALIDARG );
        }
        for ( UINT index : source.Indices ) if ( index >= source.Vertices.size() ) return Error( "Invalid ray tracing triangle indices", E_INVALIDARG );
        const UINT64 vertexBytes = source.Vertices.size() * static_cast<UINT64>(sizeof(Vertex));
        const UINT64 indexBytes = source.Indices.size() * static_cast<UINT64>(sizeof(UINT));
        if ( vertexBytes + indexBytes > GeometryBudget - std::min(GeometryBytes - mesh.Bytes, GeometryBudget) )
            return Error( "The ray tracing geometry budget was exceeded", E_OUTOFMEMORY );
        GpuMesh replacement;
        replacement.Index = mesh.Index; replacement.Revision = source.Revision;
        replacement.VertexCount = source.Vertices.size(); replacement.IndexCount = source.Indices.size();
        ComPtr<ID3D12Resource> vertexUpload, indexUpload;
        if ( !Upload(slot, source.Vertices.data(), vertexBytes, vertexUpload) || !Upload(slot, source.Indices.data(), indexBytes, indexUpload) ||
            !Buffer(vertexBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, replacement.Vertices) ||
            !Buffer(indexBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, replacement.Indices) ) return false;
        auto* list = slot.List.Get();
        list->CopyBufferRegion( replacement.Vertices.Get(), 0, vertexUpload.Get(), 0, vertexBytes );
        list->CopyBufferRegion( replacement.Indices.Get(), 0, indexUpload.Get(), 0, indexBytes );
        Transition( list, replacement.Vertices.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        Transition( list, replacement.Indices.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
        D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        // All triangles stay non-opaque so animated material masks can change
        // without rebuilding their geometric acceleration structure.
        geometry.Triangles.VertexBuffer.StartAddress = replacement.Vertices->GetGPUVirtualAddress();
        geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
        geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.VertexCount = static_cast<UINT>(source.Vertices.size());
        geometry.Triangles.IndexBuffer = replacement.Indices->GetGPUVirtualAddress();
        geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        geometry.Triangles.IndexCount = static_cast<UINT>(source.Indices.size());
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        inputs.NumDescs = 1; inputs.pGeometryDescs = &geometry;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes = {};
        Device->GetRaytracingAccelerationStructurePrebuildInfo( &inputs, &sizes );
        if ( !sizes.ResultDataMaxSizeInBytes || !sizes.ScratchDataSizeInBytes ) return Error( "Invalid ray tracing acceleration size", E_INVALIDARG );
        replacement.Bytes = vertexBytes + indexBytes + sizes.ResultDataMaxSizeInBytes;
        if ( replacement.Bytes > GeometryBudget - std::min(GeometryBytes - mesh.Bytes, GeometryBudget) )
            return Error( "The ray tracing acceleration budget was exceeded", E_OUTOFMEMORY );
        ComPtr<ID3D12Resource> scratch;
        if ( !Buffer(sizes.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                replacement.Acceleration, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
            !Buffer(sizes.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                scratch, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ) return false;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
        build.Inputs = inputs;
        build.DestAccelerationStructureData = replacement.Acceleration->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure( &build, 0, nullptr );
        UAVBarrier( list, replacement.Acceleration.Get() );
        slot.Lifetime.push_back(scratch);
        if ( mesh.Vertices ) slot.Lifetime.push_back(mesh.Vertices);
        if ( mesh.Indices ) slot.Lifetime.push_back(mesh.Indices);
        if ( mesh.Acceleration ) slot.Lifetime.push_back(mesh.Acceleration);
        GeometryBytes = GeometryBytes - mesh.Bytes + replacement.Bytes;
        mesh = std::move(replacement);
        BufferDescriptor( mesh.Vertices.Get(), static_cast<UINT>(mesh.VertexCount), sizeof(Vertex), 7 + MaxTextures + mesh.Index );
        BufferDescriptor( mesh.Indices.Get(), static_cast<UINT>(mesh.IndexCount), sizeof(UINT), 7 + MaxTextures + MaxMeshes + mesh.Index );
        return true;
    }

    bool BuildScene( const Scene& scene, Slot& slot ) {
        if ( scene.Instances.empty() || scene.Instances.size() > 0xffffffu || scene.Meshes.size() > MaxMeshes )
            return Error( "The ray tracing scene is empty or exceeds its instance limits", E_INVALIDARG );
        std::vector<MeshData> materials( std::max(NextMesh, 1u) );
        for ( const auto& source : scene.Meshes ) {
            auto found = Meshes.find(source.Id);
            if ( found == Meshes.end() ) {
                if ( NextMesh >= MaxMeshes ) return Error( "The ray tracing mesh limit was exceeded", E_OUTOFMEMORY );
                GpuMesh created; created.Index = NextMesh++;
                found = Meshes.emplace(source.Id, std::move(created)).first;
                materials.resize(NextMesh);
            }
            auto& mesh = found->second;
            if ( !mesh.Acceleration || mesh.Revision != source.Revision || mesh.VertexCount != source.Vertices.size() || mesh.IndexCount != source.Indices.size() ) {
                if ( !BuildMesh(source, mesh, slot) ) return false;
            }
            MeshData& material = materials[mesh.Index];
            if ( !Finite(source.Surface.Color, 4) || !std::isfinite(source.Surface.AlphaCutoff) )
                return Error( "Invalid ray tracing material", E_INVALIDARG );
            memcpy(material.Color, source.Surface.Color, sizeof(material.Color));
            material.AlphaCutoff = std::clamp(source.Surface.AlphaCutoff, 0.0f, 1.0f);
            material.Flags = (source.Surface.AlphaTest ? 1u : 0u) | (source.Surface.DoubleSided ? 2u : 0u);
            if ( !MaterialTexture(source.Surface.Texture.Get(), material.TextureIndex) ) return false;
        }
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
        std::vector<InstanceData> instanceData;
        instances.reserve(scene.Instances.size()); instanceData.reserve(scene.Instances.size());
        for ( const auto& source : scene.Instances ) {
            auto found = Meshes.find(source.MeshId);
            if ( found == Meshes.end() || !found->second.Acceleration || !Finite(source.Transform, 12) )
                return Error( "Invalid ray tracing scene instance", E_INVALIDARG );
            D3D12_RAYTRACING_INSTANCE_DESC instance = {};
            memcpy(instance.Transform, source.Transform, sizeof(instance.Transform));
            instance.InstanceID = static_cast<UINT>(instanceData.size());
            instance.InstanceMask = 255;
            instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            instance.AccelerationStructure = found->second.Acceleration->GetGPUVirtualAddress();
            instances.push_back(instance);
            instanceData.push_back({found->second.Index, source.Color, {0, 0}});
        }
        ComPtr<ID3D12Resource> materialsUpload, dataUpload, instancesUpload;
        if ( !Upload(slot, materials.data(), materials.size() * sizeof(MeshData), materialsUpload) ||
            !Upload(slot, instanceData.data(), instanceData.size() * sizeof(InstanceData), dataUpload) ||
            !Upload(slot, instances.data(), instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), instancesUpload) ) return false;
        // Copy the CPU descriptor cache into this slot's shader-visible heap
        // below. Earlier frames retain their own descriptors and upload buffers.
        BufferDescriptor(materialsUpload.Get(), static_cast<UINT>(materials.size()), sizeof(MeshData), 4);
        BufferDescriptor(dataUpload.Get(), static_cast<UINT>(instanceData.size()), sizeof(InstanceData), 5);
        const bool changed = LastInstances.size() != instances.size() ||
            memcmp(LastInstances.data(), instances.data(), instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC)) != 0;
        if ( TopLevel && !changed ) return true;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        inputs.NumDescs = static_cast<UINT>(instances.size());
        inputs.InstanceDescs = instancesUpload->GetGPUVirtualAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes = {};
        Device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
        if ( !sizes.ResultDataMaxSizeInBytes || !sizes.ScratchDataSizeInBytes ) return Error( "Invalid top-level ray tracing acceleration size", E_INVALIDARG );
        if ( TopLevelSize < sizes.ResultDataMaxSizeInBytes ) {
            if ( TopLevel ) slot.Lifetime.push_back(TopLevel);
            if ( !Buffer(sizes.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                TopLevel, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ) return false;
            TopLevelSize = sizes.ResultDataMaxSizeInBytes;
        }
        if ( TopLevelScratchSize < sizes.ScratchDataSizeInBytes ) {
            if ( TopLevelScratch ) slot.Lifetime.push_back(TopLevelScratch);
            if ( !Buffer(sizes.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                TopLevelScratch, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ) return false;
            TopLevelScratchSize = sizes.ScratchDataSizeInBytes;
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
        build.Inputs = inputs;
        build.DestAccelerationStructureData = TopLevel->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = TopLevelScratch->GetGPUVirtualAddress();
        // The same TLAS and scratch allocation are reused on this ordered queue.
        // Make its previous traversal/build finish before overwriting either.
        UAVBarrier(slot.List.Get(), TopLevel.Get());
        UAVBarrier(slot.List.Get(), TopLevelScratch.Get());
        slot.List->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        UAVBarrier(slot.List.Get(), TopLevel.Get());
        LastInstances = std::move(instances);
        return true;
    }

    bool Resize( const Frame& frame ) {
        ID3D11Texture2D* sources[] = { frame.Depth, frame.Normals, frame.Specular, frame.Diffuse };
        D3D11_TEXTURE2D_DESC sourceDescriptions[4] = {};
        for ( UINT i = 0; i < 4; ++i ) {
            if ( !sources[i] ) return Error( "Missing deferred ray tracing texture", E_INVALIDARG );
            sources[i]->GetDesc(&sourceDescriptions[i]);
            if ( sourceDescriptions[i].SampleDesc.Count != 1 || sourceDescriptions[i].ArraySize != 1 || !sourceDescriptions[i].Width || !sourceDescriptions[i].Height )
                return Error( "Unsupported deferred ray tracing texture", E_INVALIDARG );
            if ( i && (sourceDescriptions[i].Width != sourceDescriptions[0].Width || sourceDescriptions[i].Height != sourceDescriptions[0].Height) )
                return Error( "Mismatched deferred ray tracing texture sizes", E_INVALIDARG );
        }
        const UINT divisor = std::clamp(frame.ResolutionDivisor, 1u, 4u);
        const UINT width = (sourceDescriptions[0].Width + divisor - 1) / divisor;
        const UINT height = (sourceDescriptions[0].Height + divisor - 1) / divisor;
        bool resize = SunOutput.Width != width || SunOutput.Height != height;
        const DXGI_FORMAT formats[] = { DXGI_FORMAT_R32_FLOAT, sourceDescriptions[1].Format, sourceDescriptions[2].Format, sourceDescriptions[3].Format };
        for ( UINT i = 0; i < 4; ++i ) resize |= Inputs[i].Width != sourceDescriptions[i].Width || Inputs[i].Height != sourceDescriptions[i].Height || Inputs[i].Format != formats[i];
        if ( !resize ) return true;
        if ( !WaitIdle() ) return false;
        for ( UINT i = 0; i < 4; ++i ) {
            if ( !Shared(sourceDescriptions[i].Width, sourceDescriptions[i].Height, formats[i], 1, D3D12_RESOURCE_FLAG_NONE, Inputs[i]) ) return false;
            TextureDescriptor(Inputs[i], i);
        }
        if ( !Shared(width, height, DXGI_FORMAT_R32_FLOAT, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, SunOutput) ||
            !Shared(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, ReflectionOutput) ) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = SunOutput.Format;
        Device->CreateUnorderedAccessView(SunOutput.Resource.Get(), nullptr, &uav, CPU(SrvCount));
        uav.Format = ReflectionOutput.Format;
        Device->CreateUnorderedAccessView(ReflectionOutput.Resource.Get(), nullptr, &uav, CPU(SrvCount + 1));
        return true;
    }

    bool UpdateEnvironment( ID3D11ShaderResourceView* source ) {
        if ( source == EnvironmentSource.Get() && Environment.Resource ) return true;
        if ( !WaitIdle() ) return false;
        UINT width = 1, height = 1;
        if ( source ) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
            source->GetDesc(&srv);
            if ( srv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURECUBE ) return Error("Unsupported ray tracing environment view", E_INVALIDARG);
            ComPtr<ID3D11Resource> resource;
            ComPtr<ID3D11Texture2D> texture;
            source->GetResource(resource.GetAddressOf());
            HRESULT hr = resource.As(&texture);
            if ( FAILED(hr) ) return Error("Invalid ray tracing environment texture", hr);
            D3D11_TEXTURE2D_DESC description = {};
            texture->GetDesc(&description);
            width = std::max(1u, description.Width >> srv.TextureCube.MostDetailedMip);
            height = std::max(1u, description.Height >> srv.TextureCube.MostDetailedMip);
        }
        if ( !Shared(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 6, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, Environment) || !Blit(source, Environment, true) ) return false;
        TextureDescriptor(Environment, 6, true);
        EnvironmentSource = source;
        return true;
    }

    bool Render( const Scene& scene, const Frame& frame ) {
        ValidOutput = false;
        if ( !Initialized ) return false;
        if ( !scene.Valid ) {
            Status = scene.Status.empty() ? "The ray tracing scene is incomplete. Using the existing renderer." : scene.Status;
            return false;
        }
        if ( !frame.Shadows && !frame.Reflections ) return false;
        if ( !Finite(frame.InverseProjection, 16) || !Finite(frame.InverseView, 16) || !Finite(frame.Camera, 3) ||
            !Finite(frame.SunDirection, 3) || !Finite(frame.SunColor, 3) || !Finite(frame.AmbientColor, 3) ||
            !std::isfinite(frame.SunStrength) || !std::isfinite(frame.MaxDistance) || frame.MaxDistance <= 0 ||
            !std::isfinite(frame.RayBias) || frame.RayBias <= 0 ) return Error("Invalid ray tracing frame constants", E_INVALIDARG);
        if ( !HaveGeneration || Generation != scene.Generation ) {
            if ( !ResetScene() ) return false;
            Generation = scene.Generation; HaveGeneration = true;
        }
        if ( !Resize(frame) || !UpdateEnvironment(frame.EnvironmentCube) ) return false;
        Slot& slot = Slots[NextSlot];
        if ( !WaitFor(slot.Completion) ) return false;
        slot.Lifetime.clear();
        HRESULT hr = slot.Allocator->Reset();
        if ( SUCCEEDED(hr) ) hr = slot.List->Reset(slot.Allocator.Get(), Pipeline.Get());
        if ( FAILED(hr) ) return Error("Could not reset ray tracing commands", hr);
        struct AbortUnsubmitted {
            Impl* Backend;
            Slot* Frame;
            bool Submitted = false;
            ~AbortUnsubmitted() {
                if ( Submitted ) return;
                Frame->List->Close();
                // No newly recorded BLAS was built if this command list was
                // abandoned. The next attempt must finish prior GPU work and
                // discard the cache before inspecting any acceleration entry.
                // Do not allocate or wait while unwinding an allocation failure.
                Backend->HaveGeneration = false;
                Backend->ValidOutput = false;
            }
        } abort = { this, &slot };
        if ( !BuildScene(scene, slot) ) { slot.List->Close(); return false; }
        if ( !slot.DescriptorsInitialized ) {
            Device->CopyDescriptorsSimple(DescriptorCount, slot.Descriptors->GetCPUDescriptorHandleForHeapStart(),
                Descriptors->GetCPUDescriptorHandleForHeapStart(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            slot.DescriptorsInitialized = true;
        } else {
            auto copyRange = [&](UINT start, UINT count) {
                if ( !count ) return;
                auto destination = slot.Descriptors->GetCPUDescriptorHandleForHeapStart();
                destination.ptr += static_cast<SIZE_T>(start) * DescriptorStride;
                Device->CopyDescriptorsSimple(count, destination, CPU(start), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            };
            copyRange(0, 8 + static_cast<UINT>(Textures.size()));
            copyRange(7 + MaxTextures, NextMesh);
            copyRange(7 + MaxTextures + MaxMeshes, NextMesh);
            copyRange(SrvCount, 2);
        }
        ID3D11Texture2D* sources[] = {frame.Depth, frame.Normals, frame.Specular, frame.Diffuse};
        for ( UINT i = 0; i < 4; ++i ) Context11->CopyResource(Inputs[i].Texture.Get(), sources[i]);
        FrameConstants constants = {};
        memcpy(constants.InverseProjection, frame.InverseProjection, sizeof(constants.InverseProjection));
        memcpy(constants.InverseView, frame.InverseView, sizeof(constants.InverseView));
        memcpy(constants.Camera, frame.Camera, sizeof(constants.Camera)); constants.RayBias = frame.RayBias;
        memcpy(constants.SunDirection, frame.SunDirection, sizeof(constants.SunDirection)); constants.MaxDistance = frame.MaxDistance;
        memcpy(constants.SunColor, frame.SunColor, sizeof(constants.SunColor)); constants.SunStrength = frame.SunStrength;
        memcpy(constants.AmbientColor, frame.AmbientColor, sizeof(constants.AmbientColor));
        constants.Flags = (frame.Shadows ? 1u : 0u) | (frame.Reflections ? 2u : 0u) | (frame.EnvironmentCube ? 4u : 0u);
        constants.OutputSize[0] = SunOutput.Width; constants.OutputSize[1] = SunOutput.Height;
        constants.InputSize[0] = Inputs[0].Width; constants.InputSize[1] = Inputs[0].Height;
        ComPtr<ID3D12Resource> constantUpload;
        if ( !Upload(slot, &constants, sizeof(constants), constantUpload) ) { slot.List->Close(); return false; }
        const UINT64 input = ++InputValue;
        hr = Context11Fences->Signal(InputFence11.Get(), input);
        if ( FAILED(hr) ) { slot.List->Close(); return Error("Could not submit ray tracing inputs", hr); }
        Context11->Flush();
        hr = Queue->Wait(InputFence.Get(), input);
        if ( FAILED(hr) ) { slot.List->Close(); return Error("Could not wait for deferred ray tracing inputs", hr); }
        auto* list = slot.List.Get();
        for ( auto& texture : Inputs ) Transition(list, texture.Resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(list, SunOutput.Resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(list, ReflectionOutput.Resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetComputeRootSignature(RootSignature.Get());
        ID3D12DescriptorHeap* heaps[] = {slot.Descriptors.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootConstantBufferView(0, constantUpload->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(1, TopLevel->GetGPUVirtualAddress());
        list->SetComputeRootDescriptorTable(2, GPU(slot, 0));
        list->SetComputeRootDescriptorTable(3, GPU(slot, SrvCount));
        list->Dispatch((SunOutput.Width + 7) / 8, (SunOutput.Height + 7) / 8, 1);
        UAVBarrier(list, SunOutput.Resource.Get()); UAVBarrier(list, ReflectionOutput.Resource.Get());
        Transition(list, SunOutput.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        Transition(list, ReflectionOutput.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        for ( auto& texture : Inputs ) Transition(list, texture.Resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        hr = list->Close();
        if ( FAILED(hr) ) return Error("Could not finish ray tracing commands", hr);
        ID3D12CommandList* commands[] = {list};
        Queue->ExecuteCommandLists(1, commands);
        abort.Submitted = true;
        const UINT64 completion = ++CompletionValue;
        hr = Queue->Signal(CompletionFence.Get(), completion);
        if ( FAILED(hr) ) {
            // Commands were submitted, but this slot has no new completion
            // value. Do not reset its allocator/upload lifetime on the old one.
            HaveGeneration = false;
            return Error("Could not submit the ray tracing completion fence", hr);
        }
        slot.Completion = completion;
        hr = Context11Fences->Wait(CompletionFence11.Get(), completion);
        if ( FAILED(hr) ) return Error("Could not read ray tracing output in Direct3D 11", hr);
        NextSlot = (NextSlot + 1) % FrameCount;
        ValidOutput = true;
        std::ostringstream status;
        status << "Experimental DXR shadows and reflections are active for the supplied scene ("
            << scene.Meshes.size() << " meshes, " << scene.Instances.size() << " instances, "
            << ((GeometryBytes + 1024 * 1024 - 1) / (1024 * 1024)) << " MiB geometry memory, "
            << Textures.size() << " material textures, " << ((TextureBytes + 1024 * 1024 - 1) / (1024 * 1024))
            << " MiB shared material texture memory).";
        Status = status.str();
        return true;
    }
};

D3D12RayTracing::D3D12RayTracing() : Backend(std::make_unique<Impl>()) {}
D3D12RayTracing::~D3D12RayTracing() = default;
bool D3D12RayTracing::Initialize( ID3D11Device1* device, ID3D11DeviceContext1* context, IDXGIAdapter* adapter ) {
    return Backend->Initialize(device, context, adapter);
}
bool D3D12RayTracing::Render( const Scene& scene, const Frame& frame ) { return Backend->Render(scene, frame); }
void D3D12RayTracing::ResetScene() { Backend->ResetScene(); }
const std::string& D3D12RayTracing::GetStatus() const { return Backend->Status; }
ID3D11ShaderResourceView* D3D12RayTracing::GetSunShadowSRV() const { return Backend->ValidOutput ? Backend->SunOutput.SRV.Get() : nullptr; }
ID3D11ShaderResourceView* D3D12RayTracing::GetReflectionSRV() const { return Backend->ValidOutput ? Backend->ReflectionOutput.SRV.Get() : nullptr; }

} // namespace gd3d11rt
