#include "pch.h"
#include "RayTracingScene.h"

#include "GothicAPI.h"
#include "WorldObjects.h"
#include "zCMaterial.h"
#include "zCMorphMesh.h"
#include "zCProgMeshProto.h"
#include "zCTexture.h"
#include "zCVob.h"
#include "D3D7/MyDirectDrawSurface7.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace gd3d11rt {
namespace {

constexpr uint64_t CpuSnapshotBudget = 256ull * 1024 * 1024;

// Gothic is a 32-bit process. Count retained vector capacities as well as the
// old/new allocations that coexist during reserve(), not just live triangles.
struct SnapshotBudget {
    uint64_t Bytes = 0;
    bool Exceeded = false;

    template<class T> bool Account( const std::vector<T>& buffer ) {
        if ( buffer.capacity() > (CpuSnapshotBudget - Bytes) / sizeof( T ) ) {
            Exceeded = true;
            return false;
        }
        Bytes += uint64_t( buffer.capacity() ) * sizeof( T );
        return true;
    }

    template<class T> void Release( const std::vector<T>& buffer ) {
        Bytes -= uint64_t( buffer.capacity() ) * sizeof( T );
    }

    template<class T> bool Reserve( std::vector<T>& buffer, size_t count ) {
        if ( count <= buffer.capacity() ) return true;
        // reserve may allocate before freeing the existing buffer.
        if ( count > (CpuSnapshotBudget - Bytes) / sizeof( T ) ) {
            Exceeded = true;
            return false;
        }
        const uint64_t oldBytes = uint64_t( buffer.capacity() ) * sizeof( T );
        buffer.reserve( count );
        Bytes += uint64_t( buffer.capacity() ) * sizeof( T ) - oldBytes;
        return true;
    }

    template<class T> bool Append( std::vector<T>& buffer ) {
        if ( buffer.size() < buffer.capacity() ) return true;
        const size_t capacity = buffer.capacity();
        return Reserve( buffer, (std::max)( size_t( 8 ), capacity + (std::max)( size_t( 1 ), capacity / 2 ) ) );
    }
};

template<class T> struct ScopedVectorBudget {
    SnapshotBudget& Budget;
    const std::vector<T>& Buffer;
    ~ScopedVectorBudget() { Budget.Release( Buffer ); }
};

bool CopyInstanceTransform( const XMFLOAT4X4& world, float* target ) {
    // Gothic's CPU matrices store the column-vector transform (translation in
    // _14/_24/_34). HLSL's default matrix packing transposes these bytes for its
    // row-vector mul(). DXR expects the original first three rows directly.
    const float* source = &world._11;
    for ( size_t i = 0; i < 16; ++i ) {
        if ( !std::isfinite( source[i] ) ) return false;
    }
    if ( std::abs( world._41 ) > 0.00001f || std::abs( world._42 ) > 0.00001f ||
        std::abs( world._43 ) > 0.00001f || std::abs( world._44 - 1.0f ) > 0.00001f ) {
        return false; // A projective matrix cannot be represented by a DXR instance.
    }
    const double determinant = double( world._11 ) * (double( world._22 ) * world._33 - double( world._23 ) * world._32) -
        double( world._12 ) * (double( world._21 ) * world._33 - double( world._23 ) * world._31) +
        double( world._13 ) * (double( world._21 ) * world._32 - double( world._22 ) * world._31);
    if ( !std::isfinite( determinant ) || std::abs( determinant ) < 1e-12 ) return false;
    std::copy_n( source, 12, target );
    return true;
}

bool CopyVertex( const ExVertexStruct& source, Vertex& target ) {
    target = {};
    target.Position[0] = source.Position.x;
    target.Position[1] = source.Position.y;
    target.Position[2] = source.Position.z;
    for ( float coordinate : target.Position ) {
        if ( !std::isfinite( coordinate ) ) return false;
    }
    target.Normal[0] = source.Normal.x;
    target.Normal[1] = source.Normal.y;
    target.Normal[2] = source.Normal.z;
    if ( !std::isfinite( target.Normal[0] ) || !std::isfinite( target.Normal[1] ) ||
        !std::isfinite( target.Normal[2] ) ) {
        target.Normal[0] = target.Normal[2] = 0;
        target.Normal[1] = 1;
    }
    target.TexCoord[0] = std::isfinite( source.TexCoord.x ) ? source.TexCoord.x : 0;
    target.TexCoord[1] = std::isfinite( source.TexCoord.y ) ? source.TexCoord.y : 0;
    target.Color = source.Color;
    return true;
}

bool CopyIndices( const MeshInfo& source, size_t vertexCount, std::vector<uint32_t>& target,
    SnapshotBudget* budget = nullptr ) {
    target.clear();
    if ( budget ) {
        if ( !budget->Reserve( target, source.Indices.size() ) ) return false;
    } else target.reserve( source.Indices.size() );
    for ( size_t i = 0; i + 2 < source.Indices.size(); i += 3 ) {
        const uint32_t a = source.Indices[i], b = source.Indices[i + 1], c = source.Indices[i + 2];
        if ( a >= vertexCount || b >= vertexCount || c >= vertexCount || a == b || b == c || c == a ) continue;
        target.push_back( a );
        target.push_back( b );
        target.push_back( c );
    }
    return !target.empty();
}

bool CopyVertices( const MeshInfo& source, MeshVisualInfo* visual, std::vector<Vertex>& target,
    SnapshotBudget* budget = nullptr ) {
    target.clear();
    if ( visual && visual->MorphMeshVisual ) {
        // The cached MeshInfo vertices remain in the original pose: Gothic's
        // morph update writes only its GPU buffer. Read the current wedge pose.
        zCProgMeshProto* morph = reinterpret_cast<zCMorphMesh*>(visual->MorphMeshVisual)->GetMorphMesh();
        if ( !morph || source.MeshIndex >= static_cast<unsigned int>(morph->GetNumSubmeshes()) ) return false;
        const zCSubMesh* submesh = morph->GetSubmesh( static_cast<int>(source.MeshIndex) );
        const auto* positions = morph->GetPositionList();
        if ( !submesh || !positions || !positions->Array || positions->NumInArray <= 0 ||
            !submesh->WedgeList.Array || submesh->WedgeList.NumInArray <= 0 ) return false;
        if ( budget ) {
            if ( !budget->Reserve( target, submesh->WedgeList.NumInArray ) ) return false;
        } else target.reserve( submesh->WedgeList.NumInArray );
        for ( int i = 0; i < submesh->WedgeList.NumInArray; ++i ) {
            const zTPMWedge& wedge = submesh->WedgeList.Array[i];
            if ( wedge.position >= positions->NumInArray ) return false;
            ExVertexStruct vertex{};
            vertex.Position = positions->Array[wedge.position];
            vertex.Normal = wedge.normal;
            vertex.TexCoord = wedge.texUV;
            vertex.Color = 0xffffffffu;
            Vertex converted;
            if ( !CopyVertex( vertex, converted ) ) return false;
            target.push_back( converted );
        }
    } else {
        if ( source.Vertices.empty() || source.Vertices.size() > (std::numeric_limits<uint32_t>::max)() ) return false;
        if ( budget ) {
            if ( !budget->Reserve( target, source.Vertices.size() ) ) return false;
        } else target.reserve( source.Vertices.size() );
        for ( const ExVertexStruct& vertex : source.Vertices ) {
            Vertex converted;
            if ( !CopyVertex( vertex, converted ) ) return false;
            target.push_back( converted );
        }
    }
    return target.size() >= 3;
}

bool ReadMaterial( const MeshKey& key, Material& target ) {
    if ( !key.Material ) return false;
    if ( key.Info && key.Info->MaterialType != MaterialInfo::MT_None ) return false;
    if ( key.Material->GetMatGroup() == zMAT_GROUP_WATER ) return false;
    const int alpha = key.Material->GetAlphaFunc();
    if ( alpha != zMAT_ALPHA_FUNC_MAT_DEFAULT && alpha != zMAT_ALPHA_FUNC_NONE && alpha != zMAT_ALPHA_FUNC_TEST ) return false;

    // Read animation state without advancing it a second time after rasterization.
    zCTexture* texture = key.Material->GetCurrentTexture();
    if ( !texture ) texture = key.Texture;
    if ( !texture ) return false;
    target = {};
    target.AlphaTest = alpha == zMAT_ALPHA_FUNC_TEST || texture->HasAlphaChannel();
    target.DoubleSided = target.AlphaTest;
    // The rasterized foliage/shadow paths use the renderer's default 0.5 cutoff.
    target.AlphaCutoff = 0.5f;
    if ( key.Info ) {
        target.Color[0] = key.Info->buffer.Color.x;
        target.Color[1] = key.Info->buffer.Color.y;
        target.Color[2] = key.Info->buffer.Color.z;
        target.Color[3] = key.Info->buffer.Color.w;
        for ( float& channel : target.Color ) {
            channel = std::isfinite( channel ) ? (std::max)( 0.0f, channel ) : 1.0f;
        }
    }

    // Request off-camera materials asynchronously, then retain the available
    // SRV. Never turn an unloaded alpha-test texture into an opaque rectangle.
    if ( texture->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
        MyDirectDrawSurface7* surface = texture->GetSurface();
        if ( surface && surface->IsSurfaceReady() && surface->GetEngineTexture() ) {
            target.Texture = surface->GetEngineTexture()->GetShaderResourceView();
        }
    }
    return !target.AlphaTest || target.Texture.Get() != nullptr;
}

} // namespace

struct SceneAdapter::Impl {
    struct CachedMesh {
        size_t Index = 0;
        const ExVertexStruct* VertexData = nullptr;
        const VERTEX_INDEX* IndexData = nullptr;
        size_t VertexCount = 0, IndexCount = 0;
        Microsoft::WRL::ComPtr<ID3D11Buffer> SourceVertexBuffer;
        MeshVisualInfo* Visual = nullptr;
        bool Seen = false;
    };

    Scene Current;
    Scene Rejected;
    SnapshotBudget Budget;
    std::unordered_map<const MeshInfo*, CachedMesh> Cache;
    const void* World = nullptr;
    const void* WrappedWorld = nullptr;
    uint64_t WorldGeneration = 0;
    uint64_t NextId = 1;
    std::vector<Vertex> MorphVertices;

    void Reset() {
        ++Current.Generation;
        Current.Meshes.clear();
        Current.Instances.clear();
        Cache.clear();
        std::vector<Vertex>().swap( MorphVertices );
        World = WrappedWorld = nullptr;
        WorldGeneration = 0;
        NextId = 1;
    }

    const Scene& RejectBudget() {
        // A partial snapshot must never reach DXR. Release it so the game can
        // keep rendering normally and a later smaller scene can be retried.
        Reset();
        Rejected.Generation = Current.Generation;
        Rejected.Valid = false;
        Rejected.Status = "The ray tracing CPU scene snapshot exceeds its 256 MiB memory budget";
        return Rejected;
    }

    bool CountRetainedBuffers() {
        Budget = {};
        if ( !Budget.Account( Current.Meshes ) || !Budget.Account( Current.Instances ) ||
            !Budget.Account( MorphVertices ) ) return false;
        for ( const Mesh& mesh : Current.Meshes ) {
            if ( !Budget.Account( mesh.Vertices ) || !Budget.Account( mesh.Indices ) ) return false;
        }
        return true;
    }

    uint64_t AddMesh( const MeshKey& key, MeshInfo* source, MeshVisualInfo* visual ) {
        if ( !source || source->Indices.empty() ) return 0;
        auto found = Cache.find( source );
        if ( found != Cache.end() && found->second.Seen ) return Current.Meshes[found->second.Index].Id;
        Material material;
        if ( !ReadMaterial( key, material ) ) return 0;
        const bool morph = visual && visual->MorphMeshVisual;
        const auto* vertexBuffer = source->MeshVertexBuffer ? source->MeshVertexBuffer->GetVertexBuffer().Get() : nullptr;
        const bool rebuild = found == Cache.end() || found->second.VertexData != source->Vertices.data() ||
            found->second.IndexData != source->Indices.data() || found->second.VertexCount != source->Vertices.size() ||
            found->second.IndexCount != source->Indices.size() || found->second.SourceVertexBuffer.Get() != vertexBuffer ||
            found->second.Visual != visual;
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        ScopedVectorBudget<Vertex> vertexBudget{ Budget, vertices };
        ScopedVectorBudget<uint32_t> indexBudget{ Budget, indices };
        if ( rebuild ) {
            if ( !CopyVertices( *source, visual, vertices, &Budget ) ||
                !CopyIndices( *source, vertices.size(), indices, &Budget ) ) return 0;
        } else if ( morph ) {
            if ( !CopyVertices( *source, visual, MorphVertices, &Budget ) ) return 0;
        }
        if ( found == Cache.end() ) {
            if ( !Budget.Append( Current.Meshes ) ) return 0;
            CachedMesh entry;
            entry.Index = Current.Meshes.size();
            found = Cache.emplace( source, std::move( entry ) ).first;
            Current.Meshes.emplace_back();
            Current.Meshes.back().Id = NextId++;
        }
        CachedMesh& cached = found->second;
        Mesh& mesh = Current.Meshes[cached.Index];
        if ( rebuild ) {
            Budget.Release( mesh.Vertices );
            Budget.Release( mesh.Indices );
            mesh.Vertices = std::move( vertices );
            mesh.Indices = std::move( indices );
            ++mesh.Revision;
        } else if ( morph && (MorphVertices.size() != mesh.Vertices.size() ||
            std::memcmp( MorphVertices.data(), mesh.Vertices.data(), MorphVertices.size() * sizeof( Vertex ) ) != 0) ) {
            if ( !CopyIndices( *source, MorphVertices.size(), indices, &Budget ) ||
                !Budget.Reserve( mesh.Vertices, MorphVertices.size() ) ) return 0;
            mesh.Vertices = MorphVertices;
            Budget.Release( mesh.Indices );
            mesh.Indices = std::move( indices );
            ++mesh.Revision;
        }
        if ( mesh.Surface.AlphaTest != material.AlphaTest || mesh.Surface.DoubleSided != material.DoubleSided ) ++mesh.Revision;
        mesh.Surface = std::move( material );
        cached.VertexData = source->Vertices.data();
        cached.IndexData = source->Indices.data();
        cached.VertexCount = source->Vertices.size();
        cached.IndexCount = source->Indices.size();
        cached.SourceVertexBuffer = source->MeshVertexBuffer ? source->MeshVertexBuffer->GetVertexBuffer() : nullptr;
        cached.Visual = visual;
        cached.Seen = true;
        return mesh.Id;
    }

    bool RemoveUnusedMeshes() {
        size_t used = 0;
        for ( const auto& entry : Cache ) if ( entry.second.Seen ) ++used;
        if ( used == Cache.size() ) return true;
        std::vector<CachedMesh*> byIndex;
        ScopedVectorBudget<CachedMesh*> indexBudget{ Budget, byIndex };
        if ( !Budget.Reserve( byIndex, Current.Meshes.size() ) ) return false;
        byIndex.resize( Current.Meshes.size() );
        for ( auto& entry : Cache ) byIndex[entry.second.Index] = &entry.second;
        std::vector<Mesh> live;
        ScopedVectorBudget<Mesh> liveBudget{ Budget, live };
        if ( !Budget.Reserve( live, used ) ) return false;
        for ( size_t i = 0; i < Current.Meshes.size(); ++i ) {
            CachedMesh* record = byIndex[i];
            if ( record && record->Seen ) {
                record->Index = live.size();
                live.push_back( std::move( Current.Meshes[i] ) );
            } else {
                Budget.Release( Current.Meshes[i].Vertices );
                Budget.Release( Current.Meshes[i].Indices );
            }
        }
        Budget.Release( Current.Meshes );
        Current.Meshes = std::move( live );
        for ( auto it = Cache.begin(); it != Cache.end(); ) {
            if ( !it->second.Seen ) it = Cache.erase( it );
            else ++it;
        }
        return true;
    }

    const Scene& Capture( GothicAPI& game ) {
        const WorldInfo* info = game.GetLoadedWorldInfo();
        const void* mainWorld = info ? info->MainWorld : nullptr;
        const void* wrapped = game.GetWrappedWorldMesh();
        const uint64_t generation = game.GetWorldGeometryGeneration();
        if ( mainWorld != World || wrapped != WrappedWorld || generation != WorldGeneration ) {
            Reset();
            World = mainWorld;
            WrappedWorld = wrapped;
            WorldGeneration = generation;
        }
        if ( !CountRetainedBuffers() ) return RejectBudget();
        Current.Instances.clear();
        for ( auto& entry : Cache ) entry.second.Seen = false;
        if ( !info || !info->MainWorld || !wrapped ) {
            if ( !RemoveUnusedMeshes() ) return RejectBudget();
            return Current;
        }

        // WorldMeshes excludes the SuppressedMeshes collection. No camera range,
        // frustum, occlusion or portal visibility filters apply to ray blockers.
        for ( auto& x : game.GetWorldSections() ) {
            for ( auto& y : x.second ) {
                for ( const auto& entry : y.second.WorldMeshes ) {
                    const auto cached = Cache.find( entry.second );
                    const bool firstInstance = cached == Cache.end() || !cached->second.Seen;
                    const uint64_t id = AddMesh( entry.first, entry.second, nullptr );
                    if ( Budget.Exceeded ) return RejectBudget();
                    if ( id && firstInstance ) {
                        if ( !Budget.Append( Current.Instances ) ) return RejectBudget();
                        Instance instance;
                        instance.MeshId = id;
                        Current.Instances.push_back( instance );
                    }
                }
            }
        }

        // This canonical map includes dynamic additions/moves. The per-visual
        // Instances list contains only the main camera's visible draw list.
        for ( const auto& entry : game.GetStaticVobMap() ) {
            VobInfo* vob = entry.second;
            if ( !vob || !vob->Vob || !vob->VisualInfo || vob->Vob->GetHomeWorld() != info->MainWorld ||
                !vob->Vob->GetShowMainVisual() || vob->Vob->GetVisualAlpha() ) continue;
            auto* visual = static_cast<MeshVisualInfo*>(vob->VisualInfo);
            Instance instance;
            XMFLOAT4X4 world;
            XMStoreFloat4x4( &world, vob->Vob->GetWorldMatrixXM() );
            if ( !CopyInstanceTransform( world, instance.Transform ) ) continue;
            instance.Color = vob->GroundColor;
            for ( const auto& materialMeshes : visual->MeshesByTexture ) {
                for ( MeshInfo* mesh : materialMeshes.second ) {
                    instance.MeshId = AddMesh( materialMeshes.first, mesh, visual );
                    if ( Budget.Exceeded ) return RejectBudget();
                    if ( instance.MeshId ) {
                        if ( !Budget.Append( Current.Instances ) ) return RejectBudget();
                        Current.Instances.push_back( instance );
                    }
                }
            }
        }
        if ( !RemoveUnusedMeshes() ) return RejectBudget();
        return Current;
    }
};

SceneAdapter::SceneAdapter() : Adapter( std::make_unique<Impl>() ) { Adapter->Reset(); }
SceneAdapter::~SceneAdapter() = default;
const Scene& SceneAdapter::Capture( GothicAPI& game ) { return Adapter->Capture( game ); }
void SceneAdapter::Reset() { Adapter->Reset(); }

} // namespace gd3d11rt
