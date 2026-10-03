#pragma once

#include "D3D12RayTracing.h"

class GothicAPI;

namespace gd3d11rt {

// Converts the complete loaded static scene, rather than the camera's draw list.
// Native mesh pointers never escape this adapter into the D3D12 backend.
class SceneAdapter {
public:
    SceneAdapter();
    ~SceneAdapter();
    SceneAdapter( const SceneAdapter& ) = delete;
    SceneAdapter& operator=( const SceneAdapter& ) = delete;

    const Scene& Capture( GothicAPI& game );
    // Call on world unload/reconversion, including reloads that reuse addresses.
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> Adapter;
};

} // namespace gd3d11rt
