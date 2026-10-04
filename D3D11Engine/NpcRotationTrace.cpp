#include "pch.h"
#include "NpcRotationTrace.h"
#include <cstdio>
#include <cstring>
#include <iomanip>

#if defined(BUILD_GOTHIC_2_6_fix) && !defined(BUILD_SPACER_NET)
namespace NpcRotationTrace {
namespace {
    struct Watch {
        DWORD address, originalAddress, originalControl;
        unsigned index, matrixIndex;
        bool pending;
    };
    struct Report {
        CONTEXT context;
        void* vob;
        DWORD caller, thread, hits, hitMask;
        DWORD beforeWorld[16], beforePending[16], afterWorld[16], afterPending[16];
        DWORD stack[64], frames[16], axis[3], angle;
        BYTE code[48];
        unsigned frameCount, stackBytes, codeBytes;
        bool beforeWorldValid, beforePendingValid, afterWorldValid, afterPendingValid;
        bool rejected, observedTransition;
        char reason[128];
        Watch watches[4];
        unsigned watchCount;
    };
    char targetName[256], reportPath[4 * MAX_PATH];
    bool initialized = false, teardownWarning = false;
    DWORD gameThread = 0;
    void* selected = nullptr;
    float* world = nullptr;
    float* pending = nullptr;
    PVOID handler = nullptr;
    volatile LONG stopped = 0, reportState = 0;
    bool captureAllowed = false;
    DWORD ownedMask = 0, hitCount = 0;
    Watch watches[4] = {};
    unsigned watchCount = 0;
    DWORD beforeWorld[16] = {}, beforePending[16] = {};
    bool beforeWorldValid = false, beforePendingValid = false;
    Report report = {};

    bool CopyMemorySafe( void* destination, const void* source, size_t size ) {
        if ( !source ) return false;
        __try {
            memcpy( destination, source, size );
            return true;
        } __except ( GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH ) { return false; }
    }

    bool WriteSameValueSafe( DWORD address ) {
        __try {
            volatile DWORD* value = reinterpret_cast<volatile DWORD*>( address );
            const DWORD unchanged = *value;
            *value = unchanged;
            return true;
        } __except ( GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH ) { return false; }
    }

    DWORD& DebugAddress( CONTEXT& context, unsigned index ) {
        switch ( index ) {
        case 0: return context.Dr0;
        case 1: return context.Dr1;
        case 2: return context.Dr2;
        default: return context.Dr3;
        }
    }

    bool OwnsSlot( CONTEXT& context, const Watch& watch ) {
        return DebugAddress( context, watch.index ) == watch.address
            && ( ( context.Dr7 >> ( 2 * watch.index ) ) & 3 ) == 1
            && ( ( context.Dr7 >> ( 16 + 4 * watch.index ) ) & 15 ) == 13;
    }

    void ClearOwned( CONTEXT& context ) {
        context.ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        for ( unsigned i = 0; i < watchCount; ++i ) {
            const Watch& watch = watches[i];
            if ( !OwnsSlot( context, watch ) ) continue;
            const unsigned controlShift = 16 + 4 * watch.index;
            context.Dr7 &= ~( ( 3u << ( 2 * watch.index ) ) | ( 15u << controlShift ) );
            context.Dr7 |= watch.originalControl << controlShift;
            DebugAddress( context, watch.index ) = watch.originalAddress;
            context.Dr6 &= ~( 1u << watch.index );
        }
    }

    void SaveBefore() {
        beforeWorldValid = CopyMemorySafe( beforeWorld, world, sizeof( beforeWorld ) );
        beforePendingValid = CopyMemorySafe( beforePending, pending, sizeof( beforePending ) );
    }

    void Capture( CONTEXT& context, DWORD mask, const char* reason, bool rejected,
        void* caller = nullptr, const float* axis = nullptr, float angle = 0 ) {
        report.context = context;
        report.vob = selected;
        report.thread = GetCurrentThreadId();
        report.caller = reinterpret_cast<DWORD>( caller );
        report.hits = hitCount;
        report.hitMask = mask;
        report.rejected = rejected;
        report.beforeWorldValid = beforeWorldValid;
        report.beforePendingValid = beforePendingValid;
        memcpy( report.beforeWorld, beforeWorld, sizeof( beforeWorld ) );
        memcpy( report.beforePending, beforePending, sizeof( beforePending ) );
        report.afterWorldValid = CopyMemorySafe( report.afterWorld, world, sizeof( report.afterWorld ) );
        report.afterPendingValid = CopyMemorySafe( report.afterPending, pending, sizeof( report.afterPending ) );
        CopyMemorySafe( report.axis, axis, sizeof( report.axis ) );
        memcpy( &report.angle, &angle, sizeof( angle ) );
        strncpy_s( report.reason, reason, _TRUNCATE );
        report.watchCount = watchCount;
        memcpy( report.watches, watches, sizeof( watches ) );
        report.stackBytes = CopyMemorySafe( report.stack, reinterpret_cast<void*>( context.Esp ), sizeof( report.stack ) )
            ? sizeof( report.stack ) : 0;
        report.frameCount = 0;
        DWORD frame = context.Ebp;
        while ( report.frameCount < 16 && frame >= context.Esp && frame - context.Esp < 1024 * 1024 ) {
            DWORD pair[2];
            if ( !CopyMemorySafe( pair, reinterpret_cast<void*>( frame ), sizeof( pair ) ) ) break;
            report.frames[report.frameCount++] = pair[1];
            if ( pair[0] <= frame ) break;
            frame = pair[0];
        }
        report.codeBytes = CopyMemorySafe( report.code, reinterpret_cast<void*>( context.Eip - 32 ), sizeof( report.code ) )
            ? sizeof( report.code ) : 0;
    }

    LONG CALLBACK OnException( EXCEPTION_POINTERS* exception ) {
        if ( exception->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP
            || GetCurrentThreadId() != gameThread || !ownedMask ) return EXCEPTION_CONTINUE_SEARCH;
        CONTEXT& context = *exception->ContextRecord;
        DWORD matching = 0;
        for ( unsigned i = 0; i < watchCount; ++i ) {
            if ( OwnsSlot( context, watches[i] ) ) matching |= 1u << watches[i].index;
        }
        const DWORD hit = context.Dr6 & matching;
        if ( !hit ) return EXCEPTION_CONTINUE_SEARCH;
        // Foreign breakpoints and debugger single-stepping must reach their owner.
        const bool foreignCause = ( context.Dr6 & ( 15u | ( 7u << 13 ) ) & ~hit ) != 0;
        ++hitCount;
        bool nonfinite = false, observedTransition = false;
        for ( unsigned i = 0; i < watchCount; ++i ) {
            if ( !( hit & ( 1u << watches[i].index ) ) ) continue;
            DWORD value;
            if ( captureAllowed && CopyMemorySafe( &value, reinterpret_cast<void*>( watches[i].address ), sizeof( value ) )
                && ( value & 0x7F800000u ) == 0x7F800000u ) {
                nonfinite = true;
                const Watch& watch = watches[i];
                const bool previousReadable = watch.pending ? beforePendingValid : beforeWorldValid;
                const DWORD previous = watch.pending ? beforePending[watch.matrixIndex] : beforeWorld[watch.matrixIndex];
                if ( previousReadable && ( previous & 0x7F800000u ) != 0x7F800000u ) observedTransition = true;
            }
        }
        if ( nonfinite && InterlockedCompareExchange( &reportState, 1, 0 ) == 0 ) {
            InterlockedExchange( &stopped, 1 );
            Capture( context, hit, "non-finite watched write", false );
            report.observedTransition = observedTransition;
            captureAllowed = false;
            ClearOwned( context );
            ownedMask = 0;
            InterlockedExchange( &reportState, 2 );
        } else if ( !captureAllowed ) {
            ClearOwned( context );
            ownedMask = 0;
        } else {
            SaveBefore();
            context.ContextFlags |= CONTEXT_DEBUG_REGISTERS;
            context.Dr6 &= ~hit;
        }
        return foreignCause ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_CONTINUE_EXECUTION;
    }

    struct ContextRequest { HANDLE thread; bool arm; DWORD error; };

    DWORD WINAPI ChangeContext( void* parameter ) {
        ContextRequest& request = *static_cast<ContextRequest*>( parameter );
        if ( SuspendThread( request.thread ) == static_cast<DWORD>( -1 ) ) {
            request.error = GetLastError();
            return 0;
        }
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if ( !GetThreadContext( request.thread, &context ) ) request.error = GetLastError();
        else {
            if ( request.arm ) {
                const unsigned matrixIndices[4] = { 0, 5, 10, 0 };
                watchCount = 0;
                ownedMask = 0;
                for ( unsigned requested = 0; requested < 4; ++requested ) {
                    float* address = requested < 3 ? pending : world;
                    if ( !address ) continue;
                    for ( unsigned index = 0; index < 4; ++index ) {
                        if ( context.Dr7 & ( 3u << ( 2 * index ) ) ) continue;
                        Watch& watch = watches[watchCount++];
                        watch.index = index;
                        watch.matrixIndex = matrixIndices[requested];
                        watch.pending = requested < 3;
                        watch.address = reinterpret_cast<DWORD>( address + watch.matrixIndex );
                        watch.originalAddress = DebugAddress( context, index );
                        watch.originalControl = ( context.Dr7 >> ( 16 + 4 * index ) ) & 15;
                        DebugAddress( context, index ) = watch.address;
                        context.Dr7 = ( context.Dr7 & ~( 15u << ( 16 + 4 * index ) ) )
                            | ( 13u << ( 16 + 4 * index ) ) | ( 1u << ( 2 * index ) );
                        context.Dr6 &= ~( 1u << index );
                        ownedMask |= 1u << index;
                        break;
                    }
                }
            } else ClearOwned( context );
            if ( !SetThreadContext( request.thread, &context ) ) {
                request.error = GetLastError();
                if ( request.arm ) ownedMask = 0;
            } else if ( !request.arm ) ownedMask = 0;
        }
        // No logging, allocation or game calls while the game thread is suspended.
        if ( ResumeThread( request.thread ) == static_cast<DWORD>( -1 ) && !request.error ) request.error = GetLastError();
        return 0;
    }

    bool ChangeWatches( bool arm ) {
        ContextRequest request = {};
        request.arm = arm;
        if ( !DuplicateHandle( GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
            &request.thread, 0, FALSE, DUPLICATE_SAME_ACCESS ) ) request.error = GetLastError();
        HANDLE worker = request.error ? nullptr : CreateThread( nullptr, 0, ChangeContext, &request, 0, nullptr );
        if ( !worker && !request.error ) request.error = GetLastError();
        if ( worker ) {
            WaitForSingleObject( worker, INFINITE );
            CloseHandle( worker );
        }
        if ( request.thread ) CloseHandle( request.thread );
        if ( request.error ) {
            captureAllowed = false;
            InterlockedExchange( &stopped, 1 );
            LogWarn() << "NPC rotation trace: could not change watchpoints, Win32 error=" << request.error;
            return false;
        }
        return true;
    }

    void Disarm() {
        captureAllowed = false;
        if ( ownedMask && !ChangeWatches( false ) ) {
            // Lifetime hooks call this before freeing the watched allocation.
            // A same-value write obtains the actual hardware trap context even
            // if a helper could not get/set thread context. The handler only
            // clears our matching slots and does not inspect the NPC memory.
            for ( unsigned i = 0; i < watchCount && ownedMask; ++i ) WriteSameValueSafe( watches[i].address );
            if ( ownedMask && !teardownWarning ) {
                teardownWarning = true;
                LogWarn() << "NPC rotation trace could not confirm watchpoint teardown; its pinned handler will retire matching stale traps without reading freed memory.";
            }
        }
    }

    std::string Address( DWORD address ) {
        std::ostringstream text;
        text << "0x" << std::hex << std::setw( 8 ) << std::setfill( '0' ) << address;
        HMODULE module;
        if ( address && GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<const char*>( address ), &module ) ) {
            char path[MAX_PATH] = {};
            GetModuleFileNameA( module, path, MAX_PATH );
            const char* file = strrchr( path, '\\' );
            text << " (" << ( file ? file + 1 : path ) << "+0x" << address - reinterpret_cast<DWORD>( module ) << ")";
        }
        return text.str();
    }

    void Matrix( std::ostringstream& output, const char* name, const DWORD* values, bool valid ) {
        output << name << "=";
        if ( !valid ) { output << "unreadable\n"; return; }
        for ( unsigned i = 0; i < 16; ++i ) output << ( i ? "," : "" ) << "0x" << std::hex
            << std::setw( 8 ) << std::setfill( '0' ) << values[i];
        output << "\n";
    }

    char* LastSeparator( char* path ) {
        char* back = strrchr( path, '\\' );
        char* forward = strrchr( path, '/' );
        return !back ? forward : ( !forward || back > forward ? back : forward );
    }
}

void Initialize( const char* absoluteIniPath ) {
    if ( initialized ) return;
    initialized = true;
    if ( !absoluteIniPath || !*absoluteIniPath ) return;
    const DWORD count = GetPrivateProfileStringA( "Debug", "TraceNpcRotation", "", targetName, sizeof( targetName ), absoluteIniPath );
    if ( !count ) return;
    if ( count >= sizeof( targetName ) - 1 || IsDebuggerPresent() ) {
        targetName[0] = 0;
        LogWarn() << "NPC rotation trace disabled: NPC name is too long or a debugger is attached.";
        return;
    }
    strncpy_s( reportPath, absoluteIniPath, _TRUNCATE );
    char* separator = LastSeparator( reportPath );
    if ( separator ) { *separator = 0; separator = LastSeparator( reportPath ); }
    if ( !separator ) { targetName[0] = 0; return; }
    strcpy_s( separator + 1, sizeof( reportPath ) - ( separator + 1 - reportPath ), "NpcRotationTrace.log" );
    FILE* file = fopen( reportPath, "w" );
    if ( !file ) {
        targetName[0] = 0;
        LogWarn() << "NPC rotation trace disabled: cannot open " << reportPath;
        return;
    }
    fprintf( file, "NPC rotation trace target='%s'. Coverage: up to four free hardware slots on the game thread; pending matrix diagonals 0,5,10 and world matrix element0. Other components and writer threads are not watched.\n", targetName );
    fclose( file );
    HMODULE module = nullptr;
    if ( !GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<const char*>( &Initialize ), &module ) || !( handler = AddVectoredExceptionHandler( 1, OnException ) ) ) {
        targetName[0] = 0;
        LogWarn() << "NPC rotation trace disabled: cannot pin diagnostics module or register exception handler.";
        return;
    }
    gameThread = GetCurrentThreadId();
    LogInfo() << "NPC rotation trace enabled for '" << targetName << "'; report=" << reportPath;
}

bool Enabled() { return targetName[0] && handler && !stopped; }
bool NeedsName() { return Enabled() && !selected; }

bool Matches( void* vob, const char* name ) {
    if ( !Enabled() || !vob || GetCurrentThreadId() != gameThread ) return false;
    if ( selected ) return selected == vob;
    if ( !name || strcmp( name, targetName ) != 0 ) return false;
    selected = vob;
    return true;
}

void OnMovement( void* vob, float* world16, void* collisionObject ) {
    if ( !Matches( vob, nullptr ) ) return;
    float* pending16 = collisionObject ? reinterpret_cast<float*>( static_cast<BYTE*>( collisionObject ) + 0x44 ) : nullptr;
    if ( world == world16 && pending == pending16 && ownedMask ) {
        SaveBefore();
        return;
    }
    Disarm();
    if ( stopped ) return;
    world = world16;
    pending = pending16;
    SaveBefore();
    captureAllowed = true;
    if ( !ChangeWatches( true ) ) return;
    if ( !ownedMask ) {
        captureAllowed = false;
        InterlockedExchange( &stopped, 1 );
        LogWarn() << "NPC rotation trace stopped: no free hardware watchpoint slots.";
        return;
    }
    FILE* file = fopen( reportPath, "a" );
    if ( file ) {
        fprintf( file, "armed npc=%p thread=%lu watches=%u\n", selected, gameThread, watchCount );
        for ( unsigned i = 0; i < watchCount; ++i ) fprintf( file, "DR%u address=0x%08lx %s[%u]\n", watches[i].index,
            watches[i].address, watches[i].pending ? "pending" : "world", watches[i].matrixIndex );
        fclose( file );
    }
}

void BeforeCollisionDelete( void* vob ) {
    if ( vob != selected || GetCurrentThreadId() != gameThread ) return;
    Flush();
    Disarm();
    pending = nullptr;
    world = nullptr;
}

void RemoveVob( void* vob ) {
    if ( vob != selected || GetCurrentThreadId() != gameThread ) return;
    BeforeCollisionDelete( vob );
    selected = nullptr;
}

void Reset() {
    if ( !handler || GetCurrentThreadId() != gameThread ) return;
    Flush();
    Disarm();
    selected = nullptr;
    pending = nullptr;
    world = nullptr;
}

void ReportRejectedRotation( void* vob, const char* reason, void* caller, const float* axis3, float angle ) {
    if ( !Matches( vob, nullptr ) || InterlockedCompareExchange( &reportState, 1, 0 ) != 0 ) return;
    CONTEXT context = {};
    RtlCaptureContext( &context );
    InterlockedExchange( &stopped, 1 );
    Capture( context, 0, reason, true, caller, axis3, angle );
    // These are the guard's callers; invalid input was rejected before a store.
    report.frameCount = CaptureStackBackTrace( 1, 16, reinterpret_cast<void**>( report.frames ), nullptr );
    Disarm();
    InterlockedExchange( &reportState, 2 );
    Flush();
}

void Flush() {
    if ( reportState != 2 || GetCurrentThreadId() != gameThread ) return;
    std::ostringstream output;
    output << "\nfirst_event=" << ( report.rejected ? "rejected_rotation_input" : "nonfinite_watched_store" )
        << " target='" << targetName << "' npc=" << report.vob << " thread=" << std::dec << report.thread
        << " watched_hits=" << report.hits << " reason='" << report.reason << "'\n";
    output << "Coverage: " << report.watchCount << " selected floats on this thread. Unwatched floats and other writer threads are outside coverage.\n";
    if ( report.rejected ) output << "caller=" << Address( report.caller ) << " (input rejected before write; producer may be earlier)\n";
    else {
        output << "post_write_EIP=" << Address( report.context.Eip )
            << " (normally after the store; repeated instructions can trap within the instruction, so inspect nearby bytes)\n";
        output << "finite_to_nonfinite_observed=" << report.observedTransition << "\n";
        if ( !report.observedTransition ) output << "The watched value was already non-finite or unreadable at the previous observation; the original producer may be earlier.\n";
    }
    output << "EAX=" << Address( report.context.Eax ) << " EBX=" << Address( report.context.Ebx )
        << " ECX=" << Address( report.context.Ecx ) << " EDX=" << Address( report.context.Edx )
        << " ESI=" << Address( report.context.Esi ) << " EDI=" << Address( report.context.Edi )
        << " EBP=" << Address( report.context.Ebp ) << " ESP=" << Address( report.context.Esp ) << "\n";
    output << "context_flags=0x" << std::hex << report.context.ContextFlags << "\n";
    if ( ( report.context.ContextFlags & CONTEXT_FLOATING_POINT ) == CONTEXT_FLOATING_POINT ) {
        output << "x87_control=0x" << report.context.FloatSave.ControlWord << " status=0x" << report.context.FloatSave.StatusWord
            << " tag=0x" << report.context.FloatSave.TagWord << " register_area_bytes=";
        for ( unsigned i = 0; i < sizeof( report.context.FloatSave.RegisterArea ); ++i ) output << std::setw( 2 )
            << std::setfill( '0' ) << static_cast<unsigned>( report.context.FloatSave.RegisterArea[i] ) << " ";
        output << "\n";
    }
    if ( ( report.context.ContextFlags & CONTEXT_EXTENDED_REGISTERS ) == CONTEXT_EXTENDED_REGISTERS ) {
        // Standard x86 FXSAVE layout: MXCSR at byte24, XMM0..7 at byte160.
        DWORD mxcsr;
        memcpy( &mxcsr, report.context.ExtendedRegisters + 24, sizeof( mxcsr ) );
        output << "MXCSR=0x" << mxcsr << "\n";
        for ( unsigned reg = 0; reg < 8; ++reg ) {
            DWORD bits[4];
            memcpy( bits, report.context.ExtendedRegisters + 160 + 16 * reg, sizeof( bits ) );
            output << "XMM" << std::dec << reg << "_bits=";
            for ( unsigned lane = 0; lane < 4; ++lane ) output << ( lane ? "," : "" ) << "0x" << std::hex
                << std::setw( 8 ) << std::setfill( '0' ) << bits[lane];
            output << "\n";
        }
    }
    output << std::dec;
    for ( unsigned i = 0; i < report.watchCount; ++i ) {
        const Watch& watch = report.watches[i];
        output << "DR" << watch.index << "=" << Address( watch.address ) << " " << ( watch.pending ? "pending" : "world" )
            << "[" << watch.matrixIndex << "] triggered=" << ( ( report.hitMask & ( 1u << watch.index ) ) != 0 ) << "\n";
    }
    Matrix( output, "previous_observed_world_bits", report.beforeWorld, report.beforeWorldValid );
    Matrix( output, "previous_observed_pending_bits", report.beforePending, report.beforePendingValid );
    Matrix( output, "captured_world_bits", report.afterWorld, report.afterWorldValid );
    Matrix( output, "captured_pending_bits", report.afterPending, report.afterPendingValid );
    output << "axis_bits=" << std::hex << report.axis[0] << "," << report.axis[1] << "," << report.axis[2]
        << " angle_bits=" << report.angle << "\n";
    output << "stack_frames (frame pointers may be omitted; inspect raw stack too):\n";
    for ( unsigned i = 0; i < report.frameCount; ++i ) output << Address( report.frames[i] ) << "\n";
    output << "raw_stack_words=" << std::dec << report.stackBytes / sizeof( DWORD ) << "\n";
    for ( unsigned i = 0; i < report.stackBytes / sizeof( DWORD ); ++i ) output << Address( report.stack[i] ) << "\n";
    output << "code_from=" << Address( report.context.Eip - 32 ) << " bytes=" << std::dec << report.codeBytes << "\n";
    for ( unsigned i = 0; i < report.codeBytes; ++i ) output << std::hex << std::setw( 2 ) << std::setfill( '0' )
        << static_cast<unsigned>( report.code[i] ) << ( i + 1 == report.codeBytes ? "\n" : " " );
    FILE* file = fopen( reportPath, "a" );
    if ( file ) {
        const std::string text = output.str();
        fwrite( text.data(), 1, text.size(), file );
        fclose( file );
    }
    InterlockedExchange( &reportState, 3 );
    LogWarn() << "NPC rotation trace captured " << ( report.rejected ? "rejected rotation input" : "a non-finite watched write" )
        << " for '" << targetName << "'; " << ( report.rejected ? "caller=" : "EIP after store=" )
        << Address( report.rejected ? report.caller : report.context.Eip ) << "; report=" << reportPath
        << ( file ? "" : " (could not save report file)" );
}
}
#else
namespace NpcRotationTrace {
void Initialize( const char* ) {}
bool Enabled() { return false; }
bool NeedsName() { return false; }
bool Matches( void*, const char* ) { return false; }
void OnMovement( void*, float*, void* ) {}
void BeforeCollisionDelete( void* ) {}
void RemoveVob( void* ) {}
void Reset() {}
void Flush() {}
void ReportRejectedRotation( void*, const char*, void*, const float*, float ) {}
}
#endif
