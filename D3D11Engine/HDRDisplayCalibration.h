#pragma once

#include <windows.h>
#include <cmath>
#include <vector>

// Avoid importing newer display APIs. The SDR white query is available on
// Windows 10 1709+, but its request structure can use the Win7 SDK header types.
inline bool QueryWindowsSDRWhiteNits( HWND window, float& whiteNits ) {
    using GetBufferSizesFn = LONG (WINAPI*)(UINT32, UINT32*, UINT32*);
    using QueryConfigFn = LONG (WINAPI*)(UINT32, UINT32*, DISPLAYCONFIG_PATH_INFO*,
        UINT32*, DISPLAYCONFIG_MODE_INFO*, DISPLAYCONFIG_TOPOLOGY_ID*);
    using GetDeviceInfoFn = LONG (WINAPI*)(DISPLAYCONFIG_DEVICE_INFO_HEADER*);

    const HMODULE user32 = GetModuleHandleW( L"user32.dll" );
    if ( !user32 ) {
        return false;
    }

    const auto getBufferSizes = reinterpret_cast<GetBufferSizesFn>(
        GetProcAddress( user32, "GetDisplayConfigBufferSizes" ) );
    const auto queryConfig = reinterpret_cast<QueryConfigFn>(
        GetProcAddress( user32, "QueryDisplayConfig" ) );
    const auto getDeviceInfo = reinterpret_cast<GetDeviceInfoFn>(
        GetProcAddress( user32, "DisplayConfigGetDeviceInfo" ) );
    if ( !getBufferSizes || !queryConfig || !getDeviceInfo ) {
        return false;
    }

    const HMONITOR monitor = MonitorFromWindow( window, MONITOR_DEFAULTTONEAREST );
    MONITORINFOEXW monitorInfo = {};
    monitorInfo.cbSize = sizeof( monitorInfo );
    if ( !monitor || !GetMonitorInfoW( monitor, &monitorInfo ) ) {
        return false;
    }

    struct SDRWhiteLevelRequest {
        DISPLAYCONFIG_DEVICE_INFO_HEADER header;
        ULONG SDRWhiteLevel;
    };
    static_assert( sizeof( SDRWhiteLevelRequest ) == 24, "Unexpected display configuration request layout" );

    // A monitor can be connected or disconnected between the two API calls.
    // Bound retries so a continually changing topology does not stall rendering.
    for ( unsigned int attempt = 0; attempt < 3; ++attempt ) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if ( getBufferSizes( QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount ) != ERROR_SUCCESS
            || pathCount == 0 || modeCount == 0 ) {
            return false;
        }

        std::vector<DISPLAYCONFIG_PATH_INFO> paths( pathCount );
        std::vector<DISPLAYCONFIG_MODE_INFO> modes( modeCount );
        const LONG result = queryConfig( QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
            &modeCount, modes.data(), nullptr );
        if ( result == ERROR_INSUFFICIENT_BUFFER ) {
            continue;
        }
        if ( result != ERROR_SUCCESS || pathCount > paths.size() ) {
            return false;
        }

        for ( UINT32 index = 0; index < pathCount; ++index ) {
            const DISPLAYCONFIG_PATH_INFO& path = paths[index];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
            sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sourceName.header.size = sizeof( sourceName );
            sourceName.header.adapterId = path.sourceInfo.adapterId;
            sourceName.header.id = path.sourceInfo.id;
            if ( getDeviceInfo( &sourceName.header ) != ERROR_SUCCESS
                || lstrcmpiW( sourceName.viewGdiDeviceName, monitorInfo.szDevice ) != 0 ) {
                continue;
            }

            SDRWhiteLevelRequest white = {};
            // DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL = 11. Use its ABI
            // value because newer enum constants are gated by the SDK target.
            white.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(11);
            white.header.size = sizeof( white );
            white.header.adapterId = path.targetInfo.adapterId;
            white.header.id = path.targetInfo.id;
            if ( getDeviceInfo( &white.header ) != ERROR_SUCCESS ) {
                continue;
            }

            const float nits = static_cast<float>(white.SDRWhiteLevel) * 0.08f;
            if ( std::isfinite( nits ) && nits >= 80.0f && nits <= 1000.0f ) {
                whiteNits = nits;
                return true;
            }
        }
        return false;
    }
    return false;
}
