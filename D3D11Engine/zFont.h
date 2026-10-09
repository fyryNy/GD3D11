#pragma once
#include "zSTRING.h"
#include "zCTexture.h"

class zFont {
public:
    zSTRING name;
    int height;
    zCTexture* tex;
    uint8_t width[256];
    zVEC2 fontuv1[256];
    zVEC2 fontuv2[256];

    // Call the engine entry points so Union's per-glyph scaling is retained.
    int GetFontY() const {
#if (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)) || defined(BUILD_GOTHIC_2_6_fix)
        return reinterpret_cast<int( __thiscall* )( const zFont* )>
            ( GothicMemoryLocations::zCFont::GetFontY )( this );
#else
        return height;
#endif
    }
    int GetWidth( unsigned char character ) const {
#if (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)) || defined(BUILD_GOTHIC_2_6_fix)
        return reinterpret_cast<int( __thiscall* )( const zFont*, unsigned char )>
            ( GothicMemoryLocations::zCFont::GetWidth )( this, character );
#else
        return width[character];
#endif
    }
    int GetLetterDistance() const {
#if (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)) || defined(BUILD_GOTHIC_2_6_fix)
        return reinterpret_cast<int( __thiscall* )( const zFont* )>
            ( GothicMemoryLocations::zCFont::GetLetterDistance )( this );
#else
        return 1;
#endif
    }
    int GetFontData( unsigned char character, int& glyphWidth, zVEC2& uvMin, zVEC2& uvMax ) const {
#if (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)) || defined(BUILD_GOTHIC_2_6_fix)
        return reinterpret_cast<int( __thiscall* )( const zFont*, unsigned char, int&, zVEC2&, zVEC2& )>
            ( GothicMemoryLocations::zCFont::GetFontData )( this, character, glyphWidth, uvMin, uvMax );
#else
        glyphWidth = width[character];
        uvMin = fontuv1[character];
        uvMax = fontuv2[character];
        return 1;
#endif
    }
    zCTexture* GetFontTexture() const {
#if (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)) || defined(BUILD_GOTHIC_2_6_fix)
        return reinterpret_cast<zCTexture*( __thiscall* )( const zFont* )>
            ( GothicMemoryLocations::zCFont::GetFontTexture )( this );
#else
        return tex;
#endif
    }
};
