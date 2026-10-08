#pragma once

namespace WorldMirror {
    // The Union mirror patch negates the horizontal projection scale. Shadow
    // cameras use their own projection and must keep their original winding.
    inline bool IsReflectedProjection( float horizontalScale, bool replacementCamera ) {
        return !replacementCamera && horizontalScale < 0.0f;
    }

    inline bool FrontCounterClockwise( bool requested, bool reflected ) {
        return requested != reflected;
    }
}
