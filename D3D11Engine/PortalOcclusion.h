#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <utility>
#include <vector>

// A few blocked rays cannot establish that a doorway is hidden. Subtract the
// actual opaque polygons' shadow volumes and reject only complete coverage.
class PortalOcclusion {
public:
    struct Vec3 { double x, y, z; };
    struct Polygon { std::vector<Vec3> vertices; };

    static bool IsFullyOccluded( const std::vector<Vec3>& portal, const Vec3& camera,
        const std::vector<Polygon>& blockers, size_t* remainingWork = nullptr ) {
        std::vector<const Polygon*> views;
        views.reserve( blockers.size() );
        for ( const Polygon& blocker : blockers ) views.push_back( &blocker );
        return IsFullyOccludedViews( portal, camera, views, remainingWork );
    }

    // Cached world geometry can be tested without copying its vertex arrays.
    // A shared budget bounds all portal proofs in one frame, rather than each door.
    static bool IsFullyOccludedViews( const std::vector<Vec3>& portal, const Vec3& camera,
        const std::vector<const Polygon*>& blockers, size_t* remainingWork = nullptr ) {
        if ( !Finite( camera ) || portal.size() < 3 || portal.size() > MaxVertices ||
            blockers.empty() || blockers.size() > MaxBlockers ) return false;

        Context context;
        context.camera = camera;
        context.remainingWork = remainingWork;
        if ( !Spend( context, portal.size() * portal.size() ) ) return false;
        std::vector<Vec3> points;
        if ( !RelativeVertices( portal, camera, points ) ) return false;
        Face portalFace;
        if ( !ValidateFace( points, portalFace ) ) return false;
        if ( std::abs( portalFace.distance ) <= NumericalDistance( points ) ) return false;
        context.projectionAxis = DominantAxis( portalFace.normal );
        context.nextPlaneID = points.size();

        // An uncovered interior point proves the doorway cannot be fully hidden.
        // Keep validated volumes for the exact union proof when samples are covered.
        std::vector<std::vector<Plane>> volumes;
        volumes.reserve( blockers.size() );
        Vec3 center{};
        for ( const Vec3& point : points ) {
            center.x += point.x / points.size();
            center.y += point.y / points.size();
            center.z += point.z / points.size();
        }
        std::vector<Vec3> samples{center};
        for ( const Vec3& point : points ) {
            samples.push_back( {center.x * 0.37 + point.x * 0.63,
                center.y * 0.37 + point.y * 0.63, center.z * 0.37 + point.z * 0.63} );
        }
        std::vector<bool> covered( samples.size(), false );
        const double tolerance = NumericalDistance( points );
        for ( const Polygon* blocker : blockers ) {
            if ( !blocker ) continue;
            if ( !Spend( context, blocker->vertices.size() * blocker->vertices.size() ) ) return false;
            std::vector<Plane> volume;
            if ( !BuildShadowVolume( blocker->vertices, context, volume ) ) {
                if ( context.exhausted ) return false;
                continue;
            }
            bool overlaps = true;
            for ( const Plane& plane : volume ) {
                if ( !Spend( context, points.size() + 1 ) ) return false;
                bool someInside = false;
                for ( const Vec3& point : points ) {
                    someInside = someInside || Dot( plane.normal, point ) - plane.distance >= -tolerance;
                }
                if ( !someInside ) { overlaps = false; break; }
            }
            if ( overlaps ) {
                for ( size_t i = 0; i < samples.size(); ++i ) {
                    if ( covered[i] ) continue;
                    bool inside = true;
                    if ( !Spend( context, volume.size() ) ) return false;
                    for ( const Plane& plane : volume ) {
                        inside = inside && Dot( plane.normal, samples[i] ) - plane.distance >= -tolerance;
                    }
                    covered[i] = inside;
                }
                volumes.push_back( std::move( volume ) );
            }
        }
        if ( std::find( covered.begin(), covered.end(), false ) != covered.end() ) return false;

        Fragment initial;
        for ( size_t i = 0; i < points.size(); ++i ) {
            initial.push_back( {points[i], {i, (i + points.size() - 1) % points.size()}} );
        }
        std::vector<Fragment> remaining{std::move( initial )};

        for ( const auto& volume : volumes ) {
            std::vector<Fragment> survivors;
            for ( const Fragment& fragment : remaining ) {
                Fragment inside = fragment;
                for ( const Plane& plane : volume ) {
                    if ( inside.empty() ) break;
                    Fragment outside;
                    Fragment clipped;
                    if ( !Split( inside, plane, context, clipped, outside ) ) return false;
                    if ( !outside.empty() ) {
                        survivors.push_back( std::move( outside ) );
                        if ( survivors.size() > MaxFragments ) return false;
                    }
                    inside = std::move( clipped );
                }
                // The part inside every plane lies behind this opaque polygon.
            }
            remaining = std::move( survivors );
            if ( remaining.empty() ) return true;
        }
        return false;
    }

private:
    static constexpr size_t MaxVertices = 128;
    static constexpr size_t MaxBlockers = 128;
    static constexpr size_t MaxFragments = 512;
    static constexpr size_t MaxPlanes = 4096;
    static constexpr size_t MaxWork = 262144;

    struct Face { Vec3 normal; double distance; };
    struct Plane { Vec3 normal; double distance; size_t id; bool depth; };
    struct Vertex { Vec3 point; std::vector<size_t> boundaries; };
    using Fragment = std::vector<Vertex>;
    struct Edge {
        Vec3 first, second;
        bool operator<( const Edge& other ) const {
            if ( Less( first, other.first ) ) return true;
            if ( Less( other.first, first ) ) return false;
            return Less( second, other.second );
        }
    };
    struct Context {
        Vec3 camera{};
        unsigned projectionAxis = 0;
        size_t nextPlaneID = 0, work = 0;
        bool exhausted = false;
        size_t* remainingWork = nullptr;
        std::map<Edge, Plane> edges;
    };

    static bool Spend( Context& context, size_t work ) {
        if ( work > MaxWork - context.work ||
            (context.remainingWork && work > *context.remainingWork) ) {
            context.exhausted = true;
            return false;
        }
        context.work += work;
        if ( context.remainingWork ) *context.remainingWork -= work;
        return true;
    }

    static bool Finite( const Vec3& v ) {
        return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
    }
    static bool Less( const Vec3& a, const Vec3& b ) {
        return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z;
    }
    static bool Equal( const Vec3& a, const Vec3& b ) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    static Vec3 Subtract( const Vec3& a, const Vec3& b ) {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }
    static Vec3 Scale( const Vec3& a, double s ) { return {a.x * s, a.y * s, a.z * s}; }
    static Vec3 Cross( const Vec3& a, const Vec3& b ) {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
    static double Dot( const Vec3& a, const Vec3& b ) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static double Magnitude( const Vec3& v ) {
        return (std::max)( std::abs( v.x ), (std::max)( std::abs( v.y ), std::abs( v.z ) ) );
    }
    static bool UnitCross( const Vec3& a, const Vec3& b, Vec3& normal ) {
        const double scale = (std::max)( Magnitude( a ), Magnitude( b ) );
        if ( !(scale > 0) || !std::isfinite( scale ) ) return false;
        normal = Cross( Scale( a, 1.0 / scale ), Scale( b, 1.0 / scale ) );
        const double length = std::sqrt( Dot( normal, normal ) );
        if ( !(length > 0) || !std::isfinite( length ) ) return false;
        normal = Scale( normal, 1.0 / length );
        return Finite( normal );
    }
    static bool RelativeVertices( const std::vector<Vec3>& source, const Vec3& camera,
        std::vector<Vec3>& result ) {
        if ( source.size() < 3 || source.size() > MaxVertices ) return false;
        result.reserve( source.size() );
        for ( const Vec3& point : source ) {
            if ( !Finite( point ) ) return false;
            const Vec3 relative = Subtract( point, camera );
            // Leave ample headroom for products, even on MSVC where long double
            // has double precision. Unusual input preserves visibility.
            if ( !Finite( relative ) || Magnitude( relative ) > 1.0e100 ) return false;
            if ( result.empty() || !Equal( relative, result.back() ) ) result.push_back( relative );
        }
        if ( result.size() > 1 && Equal( result.front(), result.back() ) ) result.pop_back();
        return result.size() >= 3;
    }
    static double NumericalDistance( const std::vector<Vec3>& points ) {
        double scale = 1;
        for ( const Vec3& point : points ) scale = (std::max)( scale, Magnitude( point ) );
        return scale * std::numeric_limits<double>::epsilon() * 64;
    }
    static bool ValidateFace( const std::vector<Vec3>& points, Face& face ) {
        bool foundPlane = false;
        for ( size_t i = 1; i + 1 < points.size(); ++i ) {
            if ( UnitCross( Subtract( points[i], points[0] ), Subtract( points[i + 1], points[0] ), face.normal ) ) {
                foundPlane = true;
                break;
            }
        }
        if ( !foundPlane ) return false;
        face.distance = Dot( face.normal, points[0] );
        const double tolerance = NumericalDistance( points );
        for ( const Vec3& point : points ) {
            if ( std::abs( Dot( face.normal, point ) - face.distance ) > tolerance ) return false;
        }
        // Checking every point against every edge also rejects self-intersecting
        // input, which a test of successive turns alone does not establish.
        for ( size_t i = 0; i < points.size(); ++i ) {
            const size_t next = (i + 1) % points.size();
            const Vec3 edge = Subtract( points[next], points[i] );
            if ( Magnitude( edge ) == 0 ) return false;
            const Vec3 inward = Cross( face.normal, edge );
            for ( size_t j = 0; j < points.size(); ++j ) {
                if ( j == i || j == next ) continue;
                if ( Dot( inward, Subtract( points[j], points[i] ) ) < 0 ) return false;
            }
        }
        return true;
    }
    static unsigned DominantAxis( const Vec3& normal ) {
        if ( std::abs( normal.x ) >= std::abs( normal.y ) && std::abs( normal.x ) >= std::abs( normal.z ) ) return 0;
        return std::abs( normal.y ) >= std::abs( normal.z ) ? 1 : 2;
    }
    static bool BuildShadowVolume( const std::vector<Vec3>& source, Context& context,
        std::vector<Plane>& volume ) {
        std::vector<Vec3> points;
        if ( !RelativeVertices( source, context.camera, points ) ) return false;
        Face face;
        if ( !ValidateFace( points, face ) ) return false;
        if ( std::abs( face.distance ) <= NumericalDistance( points ) ) return false;
        if ( face.distance < 0 ) {
            face.normal = Scale( face.normal, -1 );
            face.distance = -face.distance;
        }
        volume.push_back( {face.normal, face.distance, context.nextPlaneID++, true} );

        Vec3 center{};
        const double inverseCount = 1.0 / points.size();
        for ( const Vec3& point : points ) {
            center.x += point.x * inverseCount;
            center.y += point.y * inverseCount;
            center.z += point.z * inverseCount;
        }
        for ( size_t i = 0; i < points.size(); ++i ) {
            const Vec3& first = points[i];
            const Vec3& second = points[(i + 1) % points.size()];
            const Edge edge = Less( first, second ) ? Edge{first, second} : Edge{second, first};
            auto found = context.edges.find( edge );
            if ( found == context.edges.end() ) {
                if ( context.edges.size() >= MaxPlanes ) {
                    context.exhausted = true;
                    return false;
                }
                Vec3 normal;
                if ( !UnitCross( edge.first, edge.second, normal ) ) return false;
                found = context.edges.emplace( edge, Plane{normal, 0, context.nextPlaneID++, false} ).first;
            }
            Plane plane = found->second;
            const double side = Dot( plane.normal, center );
            if ( side == 0 || !std::isfinite( side ) ) return false;
            if ( side < 0 ) plane.normal = Scale( plane.normal, -1 );
            volume.push_back( plane );
        }
        return true;
    }
    static bool OnBoundary( const Vertex& vertex, size_t id ) {
        return std::find( vertex.boundaries.begin(), vertex.boundaries.end(), id ) != vertex.boundaries.end();
    }
    static Vertex Intersection( const Vertex& a, const Vertex& b, double da, double db, size_t id ) {
        const double t = da / (da - db);
        Vertex result{{a.point.x + (b.point.x - a.point.x) * t,
            a.point.y + (b.point.y - a.point.y) * t,
            a.point.z + (b.point.z - a.point.z) * t}, {id}};
        for ( size_t boundary : a.boundaries ) {
            if ( boundary != id && OnBoundary( b, boundary ) ) result.boundaries.push_back( boundary );
        }
        return result;
    }
    static void AddVertex( Fragment& fragment, Vertex vertex ) {
        if ( !fragment.empty() && Equal( fragment.back().point, vertex.point ) ) {
            for ( size_t id : vertex.boundaries ) {
                if ( !OnBoundary( fragment.back(), id ) ) fragment.back().boundaries.push_back( id );
            }
        } else {
            fragment.push_back( std::move( vertex ) );
        }
    }
    static void RemoveDegenerate( Fragment& fragment, const Context& context ) {
        if ( fragment.size() > 1 && Equal( fragment.front().point, fragment.back().point ) ) {
            for ( size_t id : fragment.back().boundaries ) {
                if ( !OnBoundary( fragment.front(), id ) ) fragment.front().boundaries.push_back( id );
            }
            fragment.pop_back();
        }
        if ( fragment.size() < 3 ) { fragment.clear(); return; }
        for ( size_t id : fragment.front().boundaries ) {
            bool shared = true;
            for ( const Vertex& vertex : fragment ) shared = shared && OnBoundary( vertex, id );
            if ( shared ) { fragment.clear(); return; }
        }
        long double area = 0;
        const auto coordinates = [&]( const Vec3& point ) {
            const Vec3 p = Subtract( point, fragment.front().point );
            return context.projectionAxis == 0 ? std::pair<double, double>{p.y, p.z} :
                context.projectionAxis == 1 ? std::pair<double, double>{p.x, p.z} :
                std::pair<double, double>{p.x, p.y};
        };
        auto previous = coordinates( fragment.back().point );
        for ( const Vertex& vertex : fragment ) {
            const auto current = coordinates( vertex.point );
            area += static_cast<long double>(previous.first) * current.second -
                static_cast<long double>(previous.second) * current.first;
            previous = current;
        }
        // No area tolerance: a real, narrow aperture must keep the room visible.
        if ( area == 0 ) fragment.clear();
    }
    static bool Split( const Fragment& source, const Plane& plane, Context& context,
        Fragment& inside, Fragment& outside ) {
        if ( !Spend( context, source.size() ) ) return false;
        std::vector<double> distances;
        distances.reserve( source.size() );
        bool anyPositive = false;
        for ( const Vertex& vertex : source ) {
            double distance = OnBoundary( vertex, plane.id ) ? 0 :
                Dot( plane.normal, vertex.point ) - plane.distance;
            if ( !std::isfinite( distance ) ) return false;
            // Dot products on a rotated coplanar doorway can round slightly
            // positive. A depth boundary needs a provable separation before it
            // can hide the doorway; side planes retain exact aperture edges.
            if ( plane.depth && distance > 0 ) {
                const double scale = (std::max)( 1.0,
                    (std::max)( Magnitude( vertex.point ), std::abs( plane.distance ) ) );
                if ( distance <= scale * std::numeric_limits<double>::epsilon() * 64 ) {
                    distance = 0;
                }
            }
            distances.push_back( distance );
            anyPositive = anyPositive || distance > 0;
        }
        // Coplanar geometry is not strictly in front of the doorway.
        if ( plane.depth && !anyPositive ) { outside = source; return true; }
        size_t previousIndex = source.size() - 1;
        for ( size_t i = 0; i < source.size(); ++i ) {
            const Vertex& current = source[i];
            const Vertex& previous = source[previousIndex];
            const double distance = distances[i], previousDistance = distances[previousIndex];
            if ( (distance > 0 && previousDistance < 0) || (distance < 0 && previousDistance > 0) ) {
                Vertex intersection = Intersection( previous, current, previousDistance, distance, plane.id );
                if ( !Finite( intersection.point ) ) return false;
                AddVertex( inside, intersection );
                AddVertex( outside, std::move( intersection ) );
            }
            Vertex vertex = current;
            if ( distance == 0 && !OnBoundary( vertex, plane.id ) ) vertex.boundaries.push_back( plane.id );
            if ( distance >= 0 ) AddVertex( inside, vertex );
            if ( distance <= 0 ) AddVertex( outside, std::move( vertex ) );
            previousIndex = i;
        }
        if ( inside.size() > MaxVertices || outside.size() > MaxVertices ) return false;
        RemoveDegenerate( inside, context );
        RemoveDegenerate( outside, context );
        return true;
    }
};
