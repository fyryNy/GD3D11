#pragma once
#include "pch.h"

/** This class can handle the occlusion-querrys for the BSP-Tree */
struct BspInfo;
struct MeshInfo;
class D3D11OcclusionQuerry {
public:
    D3D11OcclusionQuerry();
    ~D3D11OcclusionQuerry();

    /** Releases queries when all BSP nodes and their query IDs are discarded. */
    void ResetForWorld();

    /** Advances the frame counter of this */
    void AdvanceFrameCounter();

    /** Begins the occlusion-checks */
    void BeginOcclusionPass();

    /** Checks the BSP-Tree for visibility */
    void DoOcclusionForBSP( BspInfo* root );

    /** Ends the occlusion-checks */
    void EndOcclusionPass();

    /** Creates a new predication-object, or returns -1 on failure. */
    int AddPredicationObject();

    /** Creates the occlusion-node-mesh for the specific bsp-node */
    void CreateOcclusionNodeMeshFor( BspInfo* node );
private:

    /** Marks the entire subtree visible */
    void MarkTreeVisible( BspInfo* root, bool visible, bool resetCameraClip = false );

    void DebugVisualizeNodeMesh( MeshInfo* m, const XMFLOAT4& color );

    /** Simple box predicate */
    std::vector<Microsoft::WRL::ComPtr<ID3D11Predicate>> Predicates;

    /** Current frame */
    unsigned int FrameID;
};

