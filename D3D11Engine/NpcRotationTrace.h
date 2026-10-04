#pragma once

// Opt-in diagnostics for one named NPC; hardware watches cover four floats on
// the game thread when slots are free, rather than every transform component or
// every writer thread.
namespace NpcRotationTrace {
    void Initialize( const char* absoluteIniPath );
    bool Enabled();
    bool NeedsName();
    // A null name only checks the selected pointer and never selects a VOB.
    bool Matches( void* vob, const char* name );
    void OnMovement( void* vob, float* world16, void* collisionObject );
    void BeforeCollisionDelete( void* vob );
    void RemoveVob( void* vob );
    void Reset();
    void Flush();
    void ReportRejectedRotation( void* vob, const char* reason, void* caller,
        const float* axis3, float angle );
}
