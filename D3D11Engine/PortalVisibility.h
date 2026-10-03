#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

// Reconstruct Gothic's active portal rectangles without mutating its BSP renderer.
// Sector zero is outdoors; room sectors are constrained by directed portal faces.
class PortalVisibility {
public:
    struct Vec3 { float x, y, z; };
    struct Vec3d { double x, y, z; };
    struct Matrix { float m[4][4]; }; // Row-vector world position * view-projection.
    struct Portal {
        size_t from, to;
        Vec3 normal;
        float distance;
        std::vector<Vec3> vertices;
    };
    using OcclusionTest = std::function<bool( const Portal&, const std::vector<Vec3d>& )>;

    void Reset() {
        portals.clear();
        outgoing.clear();
        windows.clear();
        projected.clear();
        projectionState.clear();
        projectedWorld.clear();
        ready = false;
    }

    void Configure( size_t sectorCount, std::vector<Portal> faces ) {
        Reset();
        if ( sectorCount < 2 || faces.empty() ) return;
        for ( const Portal& face : faces ) {
            if ( face.from >= sectorCount || face.to >= sectorCount ||
                face.vertices.size() < 3 || !Finite( face.normal ) ||
                !std::isfinite( face.distance ) ||
                (face.normal.x == 0 && face.normal.y == 0 && face.normal.z == 0) ) return;
            for ( const Vec3& vertex : face.vertices ) {
                if ( !Finite( vertex ) ) return;
            }
        }
        portals = std::move( faces );
        outgoing.resize( sectorCount );
        windows.resize( sectorCount );
        projected.resize( portals.size() );
        projectionState.resize( portals.size() );
        projectedWorld.resize( portals.size() );
        for ( size_t i = 0; i < portals.size(); ++i ) outgoing[portals[i].from].push_back( i );
    }

    bool HasData() const { return !portals.empty(); }

    void Update( const Matrix& transform, const Vec3& camera, size_t cameraSector,
        const OcclusionTest& isOccluded = {} ) {
        ready = false;
        if ( !HasData() || cameraSector >= windows.size() || !Finite( camera ) ) return;
        bool nonzero = false;
        for ( const auto& row : transform.m ) {
            for ( float value : row ) {
                if ( !std::isfinite( value ) ) return;
                nonzero = nonzero || value != 0;
            }
        }
        if ( !nonzero ) return;
        matrix = transform;
        std::fill( windows.begin(), windows.end(), Rect{} );
        std::fill( projectionState.begin(), projectionState.end(), 0 );
        windows[cameraSector] = FullWindow();

        std::vector<size_t> queue{cameraSector};
        std::vector<bool> queued( windows.size(), false );
        queued[cameraSector] = true;
        for ( size_t head = 0; head < queue.size(); ++head ) {
            const size_t sector = queue[head];
            queued[sector] = false;
            const Rect incoming = windows[sector];
            for ( size_t index : outgoing[sector] ) {
                const Portal& face = portals[index];
                if ( projectionState[index] == 0 ) {
                    const double side = double( camera.x ) * face.normal.x +
                        double( camera.y ) * face.normal.y + double( camera.z ) * face.normal.z - face.distance;
                    // A small tolerance keeps a face reachable while crossing its plane.
                    projectedWorld[index].clear();
                    projected[index] = side >= -0.001 ? ProjectPortal( face,
                        isOccluded ? &projectedWorld[index] : nullptr ) : Rect{};
                    projectionState[index] = 1;
                }
                const Rect visible = Intersect( incoming, projected[index] );
                if ( !visible.Valid() ) continue;
                // Frustum visibility alone cannot expose a door behind a solid wall.
                // Test actual clipped geometry once, only for a reachable portal.
                if ( isOccluded && projectionState[index] == 1 ) {
                    projectionState[index] = 2;
                    if ( isOccluded( face, projectedWorld[index] ) ) {
                        projected[index] = {};
                        continue;
                    }
                }
                Rect& destination = windows[face.to];
                const Rect expanded = destination.Valid() ? Union( destination, visible ) : visible;
                if ( destination == expanded ) continue;
                destination = expanded;
                if ( !queued[face.to] ) {
                    queue.push_back( face.to );
                    queued[face.to] = true;
                }
            }
        }
        ready = true;
    }

    bool IsVisible( size_t sector, const Vec3& minimum, const Vec3& maximum ) const {
        // Unknown ownership and unavailable frame data preserve existing rendering.
        if ( sector == 0 || !ready || sector >= windows.size() ) return true;
        if ( !Finite( minimum ) || !Finite( maximum ) || minimum.x > maximum.x ||
            minimum.y > maximum.y || minimum.z > maximum.z ) return true;
        if ( !windows[sector].Valid() ) return false;
        return Intersect( windows[sector], ProjectBounds( minimum, maximum ) ).Valid();
    }

private:
    struct Rect {
        double left = 1, right = -1, bottom = 1, top = -1;
        bool Valid() const { return left <= right && bottom <= top; }
        bool operator==( const Rect& other ) const {
            return left == other.left && right == other.right && bottom == other.bottom && top == other.top;
        }
    };
    struct ClipVertex {
        double x, y, z, w;
        double worldX, worldY, worldZ;
    };

    static bool Finite( const Vec3& v ) {
        return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
    }
    static Rect FullWindow() { return {-1, 1, -1, 1}; }
    static Rect Intersect( const Rect& a, const Rect& b ) {
        return {(std::max)( a.left, b.left ), (std::min)( a.right, b.right ),
            (std::max)( a.bottom, b.bottom ), (std::min)( a.top, b.top )};
    }
    static Rect Union( const Rect& a, const Rect& b ) {
        return {(std::min)( a.left, b.left ), (std::max)( a.right, b.right ),
            (std::min)( a.bottom, b.bottom ), (std::max)( a.top, b.top )};
    }
    static void IncludePoint( Rect& bounds, double x, double y ) {
        if ( !bounds.Valid() ) {
            bounds = {x, x, y, y};
            return;
        }
        bounds.left = (std::min)( bounds.left, x );
        bounds.right = (std::max)( bounds.right, x );
        bounds.bottom = (std::min)( bounds.bottom, y );
        bounds.top = (std::max)( bounds.top, y );
    }
    ClipVertex Transform( const Vec3& v ) const {
        ClipVertex result;
        result.worldX = v.x;
        result.worldY = v.y;
        result.worldZ = v.z;
        double* components[] = {&result.x, &result.y, &result.z, &result.w};
        for ( size_t i = 0; i < 4; ++i ) {
            *components[i] = double( v.x ) * matrix.m[0][i] + double( v.y ) * matrix.m[1][i] +
                double( v.z ) * matrix.m[2][i] + matrix.m[3][i];
        }
        return result;
    }
    static double PlaneDistance( const ClipVertex& v, unsigned plane ) {
        switch ( plane ) {
        case 0: return v.x + v.w;
        case 1: return v.w - v.x;
        case 2: return v.y + v.w;
        case 3: return v.w - v.y;
        default: return v.w - 0.000001;
        }
    }
    static ClipVertex Lerp( const ClipVertex& a, const ClipVertex& b, double t ) {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t,
            a.worldX + (b.worldX - a.worldX) * t,
            a.worldY + (b.worldY - a.worldY) * t,
            a.worldZ + (b.worldZ - a.worldZ) * t};
    }
    Rect ProjectPortal( const Portal& face, std::vector<Vec3d>* clippedWorld ) const {
        std::vector<ClipVertex> polygon;
        polygon.reserve( face.vertices.size() + 5 );
        for ( const Vec3& v : face.vertices ) polygon.push_back( Transform( v ) );
        std::vector<ClipVertex> clipped;
        clipped.reserve( face.vertices.size() + 5 );
        // Gothic clips portal faces to the four side planes. The renderer's depth
        // planes must not hide an adjacent room when the camera approaches a door.
        // Guard the eye plane before dividing by w, including straddling portals.
        for ( unsigned plane = 0; plane < 5 && !polygon.empty(); ++plane ) {
            clipped.clear();
            ClipVertex previous = polygon.back();
            double previousDistance = PlaneDistance( previous, plane );
            for ( const ClipVertex& current : polygon ) {
                const double distance = PlaneDistance( current, plane );
                if ( (distance >= 0) != (previousDistance >= 0) ) {
                    clipped.push_back( Lerp( previous, current, previousDistance / (previousDistance - distance) ) );
                }
                if ( distance >= 0 ) clipped.push_back( current );
                previous = current;
                previousDistance = distance;
            }
            polygon.swap( clipped );
        }
        if ( polygon.empty() ) return {};
        Rect bounds;
        for ( const ClipVertex& v : polygon ) {
            IncludePoint( bounds, v.x / v.w, v.y / v.w );
            if ( clippedWorld ) clippedWorld->push_back( {v.worldX, v.worldY, v.worldZ} );
        }
        return Intersect( bounds, FullWindow() );
    }
    Rect ProjectBounds( const Vec3& minimum, const Vec3& maximum ) const {
        Rect bounds;
        unsigned behindEye = 0;
        for ( unsigned corner = 0; corner < 8; ++corner ) {
            const ClipVertex v = Transform( {(corner & 1) ? maximum.x : minimum.x,
                (corner & 2) ? maximum.y : minimum.y, (corner & 4) ? maximum.z : minimum.z} );
            if ( v.w <= 0.000001 ) {
                ++behindEye;
                continue;
            }
            IncludePoint( bounds, v.x / v.w, v.y / v.w );
        }
        if ( behindEye == 8 ) return {};
        // A box crossing the eye plane can expand far beyond its front corners.
        if ( behindEye != 0 ) return FullWindow();
        return Intersect( bounds, FullWindow() );
    }

    Matrix matrix{};
    std::vector<Portal> portals;
    std::vector<std::vector<size_t>> outgoing;
    std::vector<Rect> windows, projected;
    std::vector<unsigned char> projectionState;
    std::vector<std::vector<Vec3d>> projectedWorld;
    bool ready = false;
};
