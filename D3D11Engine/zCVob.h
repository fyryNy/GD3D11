#pragma once
#include "GothicAPI.h"
#include "HookedFunctions.h"
#include "zCArray.h"
#include "zCObject.h"
#include "zCPolygon.h"
#include "zSTRING.h"
#include <intrin.h>
#if defined(BUILD_GOTHIC_2_6_fix) && !defined(BUILD_SPACER_NET)
#include "NpcRotationTrace.h"
#endif

enum EVobType {
    zVOB_TYPE_NORMAL,
    zVOB_TYPE_LIGHT,
    zVOB_TYPE_SOUND,
    zVOB_TYPE_LEVEL_COMPONENT,
    zVOB_TYPE_SPOT,
    zVOB_TYPE_CAMERA,
    zVOB_TYPE_STARTPOINT,
    zVOB_TYPE_WAYPOINT,
    zVOB_TYPE_MARKER,
    zVOB_TYPE_SEPARATOR = 127,
    zVOB_TYPE_MOB,
    zVOB_TYPE_ITEM,
    zVOB_TYPE_NSC
};

enum EVisualCamAlignType {
    zVISUAL_CAM_ALIGN_NONE = 0,
    zVISUAL_CAM_ALIGN_YAW = 1,
    zVISUAL_CAM_ALIGN_FULL = 2
};

class zCBspLeaf;
class zCVisual;
class zCWorld;
class oCGame;

#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
extern bool haveWindAnimations;
#endif

class zCVob {
public:
    /** Hooks the functions of this Class */
    static void Hook() {
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCVobSetVisual), Hooked_SetVisual );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCVobDestructor), Hooked_Destructor );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCVobEndMovement), Hooked_EndMovement );
#if defined(BUILD_GOTHIC_2_6_fix) && !defined(BUILD_SPACER_NET)
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCVobRotateWorld), Hooked_RotateWorld );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCVobBeginMovement), Hooked_BeginMovement );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCVobDestroyCollisionObject), Hooked_DestroyCollisionObject );
        DetourAttach( &reinterpret_cast<PVOID&>(HookedFunctions::OriginalFunctions.original_zCVobSetCollisionObject), Hooked_SetCollisionObject );
#endif
    }

#if defined(BUILD_GOTHIC_2_6_fix) && !defined(BUILD_SPACER_NET)
    static bool IsRotationTraceTarget( zCVob* vob ) {
        if ( !NpcRotationTrace::Enabled() )
            return false;
        // Once selected, matching does not copy the name on every movement.
        return NpcRotationTrace::Matches( vob, nullptr )
            || (NpcRotationTrace::NeedsName() && NpcRotationTrace::Matches( vob, vob->GetName().c_str() ));
    }

    static void __fastcall Hooked_BeginMovement( zCVob* thisptr, void* unknwn ) {
        HookedFunctions::OriginalFunctions.original_zCVobBeginMovement( thisptr );
        if ( IsRotationTraceTarget( thisptr ) ) {
            void* collisionObject = *reinterpret_cast<void**>(reinterpret_cast<DWORD>(thisptr)
                + GothicMemoryLocations::zCVob::Offset_CollisionObject);
            NpcRotationTrace::OnMovement( thisptr, &thisptr->GetWorldMatrixPtr()->_11, collisionObject );
        }
    }

    static void __fastcall Hooked_DestroyCollisionObject( zCVob* thisptr, int force ) {
        const DWORD collisionClass = *reinterpret_cast<DWORD*>(reinterpret_cast<DWORD>(thisptr)
            + GothicMemoryLocations::zCVob::Offset_CollisionClass);
        // Persistent character collision objects survive EndMovement; keep
        // watching them until native code actually frees their allocation.
        if ( collisionClass && (force || *reinterpret_cast<int*>(collisionClass
            + GothicMemoryLocations::zCVob::Offset_CollisionClassIsVolatile)) )
            NpcRotationTrace::BeforeCollisionDelete( thisptr );
        HookedFunctions::OriginalFunctions.original_zCVobDestroyCollisionObject( thisptr, force );
    }

    static void __fastcall Hooked_SetCollisionObject( zCVob* thisptr, void* unknwn, void* collisionObject ) {
        NpcRotationTrace::BeforeCollisionDelete( thisptr );
        HookedFunctions::OriginalFunctions.original_zCVobSetCollisionObject( thisptr, collisionObject );
    }

    static void TraceRejectedRotation( zCVob* vob, const char* reason, void* caller,
        const XMFLOAT3& axis, float angle ) {
        NpcRotationTrace::Flush();
        if ( IsRotationTraceTarget( vob ) )
            NpcRotationTrace::ReportRejectedRotation( vob, reason, caller, &axis.x, angle );
    }

    static bool MovementFloatIsFinite( float value ) {
        // Check IEEE-754 exponent bits; floating comparisons can be removed by /fp:fast.
        const __m128i exponentMask = _mm_set1_epi32( 0x7F800000 );
        const __m128i exponent = _mm_and_si128( _mm_castps_si128( _mm_set_ss( value ) ), exponentMask );
        return _mm_cvtsi128_si32( _mm_cmpeq_epi32( exponent, exponentMask ) ) == 0;
    }

    static bool MovementMatrixIsFinite( const XMFLOAT4X4& matrix ) {
        const __m128i exponentMask = _mm_set1_epi32( 0x7F800000 );
        __m128i invalid = _mm_setzero_si128();
        for ( int row = 0; row < 4; ++row ) {
            const __m128i exponent = _mm_and_si128( _mm_castps_si128( _mm_loadu_ps( matrix.m[row] ) ), exponentMask );
            invalid = _mm_or_si128( invalid, _mm_cmpeq_epi32( exponent, exponentMask ) );
        }
        return _mm_movemask_epi8( invalid ) == 0;
    }

    static void LogMovementAnomaly( zCVob* vob, const char* reason, void* caller,
        const XMFLOAT3* axis = nullptr, const float* angle = nullptr ) {
        static LONG reports = 0;
        const LONG report = InterlockedIncrement( &reports );
        if ( report > 8 )
            return;

        void* frames[8] = {};
        const USHORT frameCount = CaptureStackBackTrace( 1, 8, frames, nullptr );
        std::ostringstream detail;
        detail << "VOB movement anomaly: " << reason << "; name='" << vob->GetName()
            << "' vob=" << static_cast<void*>(vob) << " caller=" << caller;
        if ( axis && angle )
            detail << " axis=(" << axis->x << "," << axis->y << "," << axis->z << ") angle=" << *angle;
        detail << " stack=";
        for ( USHORT i = 0; i < frameCount; ++i )
            detail << frames[i] << " ";
        if ( report == 8 )
            detail << "(further movement anomaly logs suppressed)";
        LogWarn() << detail.str();
    }

    static void __fastcall Hooked_RotateWorld( zCVob* thisptr, void* unknwn, const XMFLOAT3& axis, float angle ) {
        if ( !MovementFloatIsFinite( angle ) ) {
            TraceRejectedRotation( thisptr, "rejected non-finite RotateWorld angle", _ReturnAddress(), axis, angle );
            LogMovementAnomaly( thisptr, "rejected non-finite RotateWorld angle", _ReturnAddress(), &axis, &angle );
            return;
        }
        // Gothic returns before reading the axis for a zero-angle rotation.
        if ( angle == 0.0f ) {
            HookedFunctions::OriginalFunctions.original_zCVobRotateWorld( thisptr, axis, angle );
            return;
        }
        if ( !MovementFloatIsFinite( axis.x )
            || !MovementFloatIsFinite( axis.y ) || !MovementFloatIsFinite( axis.z ) ) {
            TraceRejectedRotation( thisptr, "rejected non-finite RotateWorld input", _ReturnAddress(), axis, angle );
            LogMovementAnomaly( thisptr, "rejected non-finite RotateWorld input", _ReturnAddress(), &axis, &angle );
            return;
        }
        const float axisLengthSquared = XMVectorGetX( XMVector3LengthSq( XMVectorSet( axis.x, axis.y, axis.z, 0.0f ) ) );
        if ( axisLengthSquared == 0.0f || !MovementFloatIsFinite( axisLengthSquared ) ) {
            TraceRejectedRotation( thisptr, "rejected RotateWorld axis that cannot be normalized", _ReturnAddress(), axis, angle );
            LogMovementAnomaly( thisptr, "rejected RotateWorld axis that cannot be normalized", _ReturnAddress(), &axis, &angle );
            return;
        }
        HookedFunctions::OriginalFunctions.original_zCVobRotateWorld( thisptr, axis, angle );
    }

    static void GuardMovementTransform( zCVob* vob, DWORD collisionObject, void* caller ) {
        XMFLOAT4X4& proposed = *reinterpret_cast<XMFLOAT4X4*>(collisionObject + GothicMemoryLocations::zCVob::Offset_CollisionTrafoNew);
        if ( MovementMatrixIsFinite( proposed ) )
            return;

        // Persist a hardware capture before containment restores the matrix.
        NpcRotationTrace::Flush();

        const XMFLOAT4X4& previous = *reinterpret_cast<const XMFLOAT4X4*>(collisionObject + GothicMemoryLocations::zCVob::Offset_CollisionTrafoOld);
        if ( !MovementMatrixIsFinite( previous ) ) {
            LogMovementAnomaly( vob, "cannot restore non-finite movement: previous transform is also invalid", caller );
            return;
        }

        const XMFLOAT3 position( proposed._14, proposed._24, proposed._34 );
        proposed = previous;
        if ( MovementFloatIsFinite( position.x ) && MovementFloatIsFinite( position.y ) && MovementFloatIsFinite( position.z ) ) {
            proposed._14 = position.x;
            proposed._24 = position.y;
            proposed._34 = position.z;
        }
        // Native EndMovement still performs collision checks, rebuilds the box and updates BSP membership.
        LogMovementAnomaly( vob, "contained invalid pending matrix by restoring previous transform", caller );
    }
#endif
    
    /** Called when this vob got it's world-matrix changed */
#ifdef BUILD_GOTHIC_1_08k
    static void __fastcall Hooked_EndMovement( zCVob* thisptr, void* unknwn ) {
        bool vobHasMoved = false;
        if ( (*reinterpret_cast<unsigned char*>(reinterpret_cast<DWORD>(thisptr) + 0xE8) & 0x03) && thisptr->GetHomeWorld() ) {
            vobHasMoved = (*reinterpret_cast<unsigned char*>(*reinterpret_cast<DWORD*>(reinterpret_cast<DWORD>(thisptr) + 0xFC) + 0x88) & 0x03);
        }

        HookedFunctions::OriginalFunctions.original_zCVobEndMovement( thisptr );

        hook_infunc

            if ( vobHasMoved ) {
                Engine::GAPI->OnVobMoved( thisptr );
            }

        hook_outfunc
    }
#else
    static void __fastcall Hooked_EndMovement( zCVob* thisptr, void* unknwn, int transformChanged_hint ) // G2 has one parameter more
    {
#if defined(BUILD_GOTHIC_2_6_fix) && !defined(BUILD_SPACER_NET)
        NpcRotationTrace::Flush();
#endif
        hook_infunc

        bool vobHasMoved = false;
        if ( (*reinterpret_cast<unsigned char*>(reinterpret_cast<DWORD>(thisptr) + 0x108) & 0x03) && thisptr->GetHomeWorld() ) {
            const DWORD collisionObject = *reinterpret_cast<DWORD*>(reinterpret_cast<DWORD>(thisptr) + 0x11C);
            if ( collisionObject ) {
                vobHasMoved = (*reinterpret_cast<unsigned char*>(collisionObject + 0x88) & 0x03);
#if defined(BUILD_GOTHIC_2_6_fix) && !defined(BUILD_SPACER_NET)
                if ( vobHasMoved && !(*reinterpret_cast<unsigned char*>(reinterpret_cast<DWORD>(thisptr)
                    + GothicMemoryLocations::zCVob::Offset_MovementState) & GothicMemoryLocations::zCVob::MASK_InsideEndMovement) ) {
                    GuardMovementTransform( thisptr, collisionObject, _ReturnAddress() );
                }
#endif
            }
        }

        HookedFunctions::OriginalFunctions.original_zCVobEndMovement( thisptr, transformChanged_hint );
#if defined(BUILD_GOTHIC_2_6_fix) && !defined(BUILD_SPACER_NET)
        NpcRotationTrace::Flush();
#endif

        if ( vobHasMoved && transformChanged_hint )
            Engine::GAPI->OnVobMoved( thisptr );

        hook_outfunc
    }
#endif

    /** Called on destruction */
    static void __fastcall Hooked_Destructor( zCVob* thisptr, void* unknwn ) {
#if defined(BUILD_GOTHIC_2_6_fix) && !defined(BUILD_SPACER_NET)
        NpcRotationTrace::RemoveVob( thisptr );
#endif
        hook_infunc

            // Notify the world. We are doing this here for safety so nothing possibly deleted remains in our world.
            Engine::GAPI->OnRemovedVob( thisptr, thisptr->GetHomeWorld() );

        hook_outfunc

        HookedFunctions::OriginalFunctions.original_zCVobDestructor( thisptr );
    }

    /** Called when this vob is about to change the visual */
    static void __fastcall Hooked_SetVisual( zCVob* thisptr, void* unknwn, zCVisual* visual ) {
        HookedFunctions::OriginalFunctions.original_zCVobSetVisual( thisptr, visual );

        if ( Engine::GAPI->IsSavingGameNow() ) {
            return;
        }

        hook_infunc

            // Notify the world
            Engine::GAPI->OnSetVisual( thisptr );

        hook_outfunc
    }

#if (defined BUILD_SPACER || defined BUILD_SPACER_NET)
    /** Returns the helper-visual for this class
        This actually uses a map to lookup the visual. Beware for performance-issues! */
    zCVisual* GetClassHelperVisual() {
        return reinterpret_cast<zCVisual*( __fastcall* )( zCVob* )>( GothicMemoryLocations::zCVob::GetClassHelperVisual )( this );
    }

    /** Returns the visual saved in this vob */
    zCVisual* GetVisual() {
        zCVisual* visual = GetMainVisual();
#if BUILD_SPACER_NET
        if ( !visual && Engine::GAPI->GetRendererState().RendererSettings.RunInSpacerNet )
#else
        if ( !visual )
#endif
            visual = GetClassHelperVisual();

        return visual;
    }
#else
    /** Returns the visual saved in this vob */
    zCVisual* GetVisual() {
        return GetMainVisual();
    }
#endif

    void _EndMovement( int p = 1 ) {
#ifdef BUILD_GOTHIC_1_08k
        reinterpret_cast<void( __fastcall* )( zCVob* )>( GothicMemoryLocations::zCVob::EndMovement )( this );
#else
        reinterpret_cast<void( __fastcall* )( zCVob*, int, int )>( GothicMemoryLocations::zCVob::EndMovement )( this, 0, p );
#endif
    }

    /** Updates the vobs transforms */
    void EndMovement() {
        _EndMovement();
    }

    /** Returns the visual saved in this vob */
    zCVisual* GetMainVisual() {
        return reinterpret_cast<zCVisual*( __fastcall* )( zCVob* )>( GothicMemoryLocations::zCVob::GetVisual )( this );
    }

    /** Returns the name of this vob */
    std::string GetName() {
        return __GetObjectName().ToChar();
    }

    /** Returns the world-position of this vob */
    XMFLOAT3 GetPositionWorld() const {
        // Get the data right off the memory to save a function call
        return XMFLOAT3( *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WorldPosX )),
            *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WorldPosY )),
            *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WorldPosZ )) );
    }

    /** Returns the world-position of this vob */
    FXMVECTOR XM_CALLCONV GetPositionWorldXM() const {
        // Get the data right off the memory to save a function call
        FXMVECTOR pos = XMVectorSet( *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WorldPosX )),
            *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WorldPosY )),
            *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WorldPosZ )), 0 );
        return pos;
    }

    /** Sets this vobs position */
    void SetPositionWorld( const XMFLOAT3& v ) {
#ifdef BUILD_SPACER
        reinterpret_cast<void( __fastcall* )( zCVob*, int, const XMFLOAT3& )>
            ( GothicMemoryLocations::zCVob::SetPositionWorld )( this, 0, v );
#endif
    }
    /** Sets this vobs position */
    void SetPositionWorldDX( const XMFLOAT3& v ) {
#ifdef BUILD_SPACER
        reinterpret_cast<void( __fastcall* )( zCVob*, int, const XMFLOAT3& )>
            ( GothicMemoryLocations::zCVob::SetPositionWorld )( this, 0, v );
#endif
    }
    /** Sets this vobs position */
    void XM_CALLCONV SetPositionWorldXM( FXMVECTOR v ) {
        XMFLOAT3 store; XMStoreFloat3( &store, v );
        SetPositionWorldDX( store );
    }

    /** Returns the local bounding box */
    zTBBox3D GetBBoxLocal() {
        zTBBox3D box;
        reinterpret_cast<void( __fastcall* )( zCVob*, int, zTBBox3D& )>( GothicMemoryLocations::zCVob::GetBBoxLocal )( this, 0, box );
        return box;
    }

    /** Return the world/global bbox of the vob */
    zTBBox3D GetBBox() {
        return *reinterpret_cast<zTBBox3D*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WorldBBOX ));
    }

    /** Returns a pointer to this vobs world-matrix */
    XMFLOAT4X4* GetWorldMatrixPtr() {
        return reinterpret_cast<XMFLOAT4X4*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WorldMatrixPtr ));
    }

    /** Copys the world matrix into the given memory location */
    void GetWorldMatrix( XMFLOAT4X4* m ) {
        *m = *GetWorldMatrixPtr();
    }

    /** Returns a copy of the world matrix */
    XMMATRIX GetWorldMatrixXM() {
        return XMLoadFloat4x4( reinterpret_cast<XMFLOAT4X4*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WorldMatrixPtr )) );
    }

    /** Returns the world-polygon right under this vob */
    zCPolygon* GetGroundPoly() {
        return *reinterpret_cast<zCPolygon**>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_GroundPoly ));
    }

    /** Returns whether this vob is currently in an indoor-location or not */
    bool IsIndoorVob() {
        if ( !GetGroundPoly() )
            return false;

        return GetGroundPoly()->GetLightmap() != nullptr;
    }

    /** Returns the world this vob resists in */
    zCWorld* GetHomeWorld() {
        return *reinterpret_cast<zCWorld**>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_HomeWorld ));
    }

    /** Returns whether this vob is currently in sleeping state or not. Sleeping state is something like a waiting (cached out) NPC */
    int GetSleepingMode() {
        unsigned int flags = *reinterpret_cast<unsigned int*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_SleepingMode ));
        return (flags & GothicMemoryLocations::zCVob::MASK_SkeepingMode);
    }
    void SetSleeping( int on ) {
        reinterpret_cast<void( __fastcall* )( zCVob*, int, int )>( GothicMemoryLocations::zCVob::SetSleeping )( this, 0, on );
    }

#if BUILD_SPACER_NET
    /** Return whether all vobs are currently rendered or not */
    static bool GetDrawVobs()
    {
        bool showHelpers = *reinterpret_cast<int*>(GothicMemoryLocations::zCVob::s_renderVobs) != 0;
        return showHelpers;
    }
#endif

#ifndef BUILD_SPACER_NET
    /** Returns whether the visual of this vob is visible */
    bool GetShowVisual() {
#ifndef BUILD_SPACER
        return GetShowMainVisual();
#else
        // Show helpers in spacer if wanted
        bool showHelpers = *reinterpret_cast<int*>(GothicMemoryLocations::zCVob::s_ShowHelperVisuals) != 0;
        return GetShowMainVisual() || showHelpers;
#endif
    }


#else
    bool GetShowVisual() {
        bool showHelpers = *reinterpret_cast<int*>(GothicMemoryLocations::zCVob::s_ShowHelperVisuals) != 0;
        if ( !showHelpers ) {
            zCVisual* visual = GetMainVisual();
            if ( !visual ) {
                visual = GetClassHelperVisual();
                if ( visual ) {
                    return false;
                }
            }
        }

        return GetShowMainVisual() || showHelpers;
    }
#endif

    /** Returns whether to show the main visual or not. Only used for the spacer */
    bool GetShowMainVisual() {
        unsigned int flags = *reinterpret_cast<unsigned int*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_Flags ));
        return (flags & GothicMemoryLocations::zCVob::MASK_ShowVisual);
    }

    /** Returns whether vob is transparent */
    bool GetVisualAlpha() {
        unsigned int flags = *reinterpret_cast<unsigned int*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_Flags ));
        return (flags & GothicMemoryLocations::zCVob::MASK_VisualAlpha);
    }

    /** Returns dynamic collision of the vob */
    bool GetDynColl() {
        unsigned int flags = *reinterpret_cast<unsigned int*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_Flags ));
        return (flags & GothicMemoryLocations::zCVob::MASK_DynColl);
    }

    /** Vob transparency */
    float GetVobTransparency() {
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_VobAlpha ));
    }

    /** Vob type */
    EVobType GetVobType() {
        return *reinterpret_cast<EVobType*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_Type ));
    }

    /** Vob parent */
    zCVob* GetVobParent() {
        if ( DWORD vobTree = *reinterpret_cast<DWORD*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_VobTree )) ) {
            if ( ( vobTree = *reinterpret_cast<DWORD*>(vobTree + 0x00) ) != 0 ) { // Read parent from vobtree
                return *reinterpret_cast<zCVob**>(vobTree + 0x10);
            }
        }
        return nullptr;
    }

    /** Alignemt to the camera */
    EVisualCamAlignType GetAlignment() {
        unsigned int flags = *reinterpret_cast<unsigned int*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_CameraAlignment ));

        //.text:00601652                 shl     eax, 1Eh
        //.text:00601655                 sar     eax, 1Eh

        flags <<= GothicMemoryLocations::zCVob::SHIFTLR_CameraAlignment;
        flags >>= GothicMemoryLocations::zCVob::SHIFTLR_CameraAlignment;

        return static_cast<EVisualCamAlignType>(flags);
    }

    zTAnimationMode GetVisualAniMode() {
#ifdef BUILD_GOTHIC_1_08k
#ifdef BUILD_1_12F
        return zVISUAL_ANIMODE_NONE;
#else
        return (haveWindAnimations ? *reinterpret_cast<zTAnimationMode*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WindAniMode )) : zVISUAL_ANIMODE_NONE);
#endif
#else
        return *reinterpret_cast<zTAnimationMode*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WindAniMode ));
#endif
    };

    float GetVisualAniModeStrength() {
#ifdef BUILD_GOTHIC_1_08k
#ifdef BUILD_1_12F
        return 0.f;
#else
        return (haveWindAnimations ? *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WindAniModeStrength )) : 0.f);
#endif
#else
        return *reinterpret_cast<float*>(THISPTR_OFFSET( GothicMemoryLocations::zCVob::Offset_WindAniModeStrength ));
#endif
    };
    
    /** Checks the inheritance chain and casts to T* if possible. Returns nullptr otherwise */
    template<class T>
    T* As() {
        zCClassDef* classDef = reinterpret_cast<zCObject*>(this)->_GetClassDef();
        if ( CheckInheritance( classDef, T::GetStaticClassDef() ) ) {
            return reinterpret_cast<T*>(this);
        }
        return nullptr;
    }
protected:

    bool CheckInheritance( const zCClassDef* def, const zCClassDef* target ) {
        while ( def ) {
            if ( def == target ) {
                return true;
            }
            def = def->baseClassDef;
        }
        return false;
    }

    zSTRING& __GetObjectName() {
        return reinterpret_cast<zSTRING&( __fastcall* )( zCVob* )>( GothicMemoryLocations::zCObject::GetObjectName )( this );
    }
};
