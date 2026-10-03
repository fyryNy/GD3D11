#include "../D3D11Engine/PortalVisibility.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
#include <vector>

using PV = PortalVisibility;
using V = PV::Vec3;
using M = PV::Matrix;
using P = PV::Portal;
static int checks = 0;
static void check(bool value, const char* description) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}
static M identity() {
    M m{};
    for (int i = 0; i < 4; ++i) m.m[i][i] = 1.f;
    return m;
}
static M perspective() {
    M m{};
    m.m[0][0] = m.m[1][1] = 1.f;
    m.m[2][2] = 100.f / 99.f;
    m.m[2][3] = 1.f;
    m.m[3][2] = -100.f / 99.f;
    return m;
}
static P quad(size_t from, size_t to, float l, float r, float b, float t, float z, bool forward = true) {
    P p{};
    p.from = from; p.to = to;
    p.normal = {0.f, 0.f, forward ? 1.f : -1.f};
    p.distance = forward ? z : -z;
    p.vertices = {{l,b,z}, {r,b,z}, {r,t,z}, {l,t,z}};
    return p;
}
static bool visible(PV& pv, size_t sector, float l = -.1f, float r = .1f, float b = -.1f, float t = .1f, float n = .5f, float f = .6f) {
    return pv.IsVisible(sector, V{l,b,n}, V{r,t,f});
}
int main() {
    const M id = identity();
    const V outside{0.f, 0.f, 0.f};
    PV pv;
    check(!pv.HasData(), "fresh instance has no sector metadata");
    pv.Update(id, outside, 0);
    check(visible(pv, 17), "missing metadata fails open");
    pv.Configure(3, {quad(0,1,-.3f,.3f,-.3f,.3f,.4f)});
    check(pv.HasData(), "valid room portal config activates metadata");
    pv.Update(id, outside, 0);
    check(visible(pv, 0), "outdoor camera keeps outdoor objects visible");
    check(visible(pv, 1), "visible doorway reaches room");
    check(!visible(pv, 1,.6f,.8f), "room object beyond doorway projection is hidden");
    check(visible(pv, 1,.25f,.4f), "doorway intersecting AABB stays visible even when center is outside");
    check(!visible(pv, 2,-5.f,5.f,-5.f,5.f,0.f,1.f), "disconnected room beam cannot become visible by protruding into outdoor space");
    check(visible(pv, 99), "unknown object sector fails open");

    pv.Configure(4, {quad(0,1,-.5f,.5f,-.5f,.5f,.3f), quad(1,2,-.2f,.2f,-.2f,.2f,.6f)});
    pv.Update(id, outside, 0);
    check(visible(pv, 2), "two connected doorways reach second room");
    check(!visible(pv, 2,.3f,.4f), "second doorway narrows visible portion of second room");
    check(!visible(pv, 3), "disconnected third room remains hidden");
    pv.Configure(3, {quad(0,1,-.8f,-.2f,-.3f,.3f,.3f), quad(1,2,.2f,.8f,-.3f,.3f,.6f)});
    pv.Update(id, outside, 0);
    check(!visible(pv, 2,.3f,.5f), "doorway outside incoming portal window does not expose further room");

    // The second path must propagate newly visible area even after room 1 was visited.
    std::vector<P> paths{quad(0,1,-.8f,-.2f,-.3f,.3f,.3f), quad(1,2,.2f,.8f,-.3f,.3f,.6f), quad(0,1,.2f,.8f,-.3f,.3f,.4f)};
    for (int order = 0; order < 2; ++order) {
        pv.Configure(3, paths); pv.Update(id, outside, 0);
        check(visible(pv,1,-.7f,-.4f), "multiple portal union preserves first visible doorway");
        check(visible(pv,1,.4f,.7f), "multiple portal union preserves second visible doorway");
        check(visible(pv,2,.3f,.5f), "later portal area propagates to downstream room");
        std::reverse(paths.begin(), paths.end());
    }
    pv.Configure(3, {quad(0,1,-.5f,.5f,-.5f,.5f,.3f), quad(1,2,-.4f,.4f,-.4f,.4f,.5f), quad(2,1,-.3f,.3f,-.3f,.3f,.6f)});
    const auto cycleStart = std::chrono::steady_clock::now();
    pv.Update(id, outside, 0);
    check(visible(pv,2), "portal cycle preserves reached room");
    check(std::chrono::steady_clock::now()-cycleStart < std::chrono::seconds(1), "portal cycle traversal terminates promptly");

    pv.Configure(2, {quad(0,1,1.5f,1.9f,-.2f,.2f,.4f)});
    pv.Update(id,outside,0);
    check(!visible(pv,1), "door outside viewport does not expose room");
    P missedCorner = quad(0,1,.95f,1.2f,.95f,1.2f,.4f);
    missedCorner.vertices = {{1.2f,.95f,.4f}, {.95f,1.2f,.4f}, {1.2f,1.2f,.4f}};
    pv.Configure(2, {missedCorner}); pv.Update(id,outside,0);
    check(!visible(pv,1,.96f,.99f,.96f,.99f), "polygon outside corner is rejected despite overlapping projected AABB");
    pv.Configure(2,{quad(0,1,-.3f,.3f,-.3f,.3f,1.2f)}); pv.Update(id,outside,0);
    check(visible(pv,1), "portal beyond far plane remains reachable like native four-side portal clipping");
    pv.Configure(2,{quad(0,1,-.3f,.3f,-.3f,.3f,.4f,false)}); pv.Update(id,outside,0);
    check(!visible(pv,1), "back facing native portal is rejected");

    P slanted = quad(0,1,-.5f,.5f,-.3f,.3f,.2f);
    slanted.normal = {-.6f,0.f,1.f}; slanted.distance = .2f;
    slanted.vertices = {{-.5f,-.3f,-.1f}, {.5f,-.3f,.5f}, {.5f,.3f,.5f}, {-.5f,.3f,-.1f}};
    pv.Configure(2,{slanted}); pv.Update(id,outside,0);
    check(visible(pv,1,.05f,.2f,-.1f,.1f,.4f,.6f), "portal crossing near plane retains clipped visible portion");
    check(visible(pv,1,-.49f,-.45f), "near-depth portion remains reachable like native four-side portal clipping");

    const M proj = perspective();
    pv.Configure(2,{quad(0,1,-.5f,.5f,-.5f,.5f,2.f)}); pv.Update(proj,outside,0);
    check(visible(pv,1,-.1f,.1f,-.1f,.1f,3.f,3.1f), "perspective doorway projects valid room window");
    check(!visible(pv,1,1.f,1.2f,-.1f,.1f,3.f,3.1f), "perspective projected room object outside doorway stays hidden");
    pv.Configure(2,{quad(0,1,-.5f,.5f,-.5f,.5f,-2.f,false)}); pv.Update(proj,outside,0);
    check(!visible(pv,1,-.1f,.1f,-.1f,.1f,3.f,3.1f), "portal behind perspective camera is rejected");
    P perspectiveNear = quad(0,1,-1.f,1.f,-.5f,.5f,1.f);
    perspectiveNear.normal={-.75f,0.f,1.f}; perspectiveNear.distance=1.f;
    perspectiveNear.vertices={{-1.f,-.5f,.25f},{1.f,-.5f,1.75f},{1.f,.5f,1.75f},{-1.f,.5f,.25f}};
    pv.Configure(2,{perspectiveNear}); pv.Update(proj,outside,0);
    check(visible(pv,1,.2f,.4f,-.1f,.1f,2.f,2.1f), "perspective near-plane portal retains front portion");
    check(visible(pv,1,-.8f,-.6f,-.1f,.1f,2.f,2.1f), "perspective portion before renderer near-depth remains reachable");

    pv.Configure(2,{quad(0,1,-.1f,.1f,-.1f,.1f,.5f)}); pv.Update(proj,outside,0);
    check(visible(pv,1,-.1f,.1f,-.1f,.1f,2.f,2.1f), "door before renderer near-depth plane still reveals next room");
    M reversed=proj; reversed.m[2][2]=-1.f/99.f; reversed.m[3][2]=100.f/99.f;
    pv.Configure(2,{quad(0,1,-.5f,.5f,-.5f,.5f,2.f)}); pv.Update(reversed,outside,0);
    check(visible(pv,1,-.1f,.1f,-.1f,.1f,3.f,3.1f), "reversed-depth projection keeps doorway visible");
    check(!visible(pv,1,1.f,1.2f,-.1f,.1f,3.f,3.1f), "reversed-depth projection still rejects outside doorway");
    check(visible(pv,1,-.1f,.1f,-.1f,.1f,.5f,2.f), "reversed-depth near-crossing AABB remains conservative");
    check(visible(pv,1,-.1f,.1f,-.1f,.1f,90.f,110.f), "reversed-depth far-crossing AABB retains portal overlap");
    pv.Configure(2,{quad(0,1,-.1f,.1f,-.1f,.1f,.5f)}); pv.Update(reversed,outside,0);
    check(visible(pv,1,-.1f,.1f,-.1f,.1f,2.f,2.1f), "reversed-depth sub-near doorway stays reachable");

    pv.Configure(3,{quad(1,0,-.3f,.3f,-.3f,.3f,.4f,false)}); pv.Update(id,V{0.f,0.f,.6f},1);
    check(visible(pv,1,.7f,.8f), "camera room receives full seed viewport");
    check(visible(pv,0), "indoor camera exposes outdoor through door");
    check(!visible(pv,2), "unconnected room remains hidden for indoor camera");
    pv.Configure(2,{quad(0,1,-.3f,.3f,-.3f,.3f,.4f)}); pv.Update(id,outside,0);
    check(visible(pv,1), "room initially visible before camera move");
    M shifted = id; shifted.m[3][0] = 3.f;
    pv.Update(shifted,V{3.f,0.f,0.f},0);
    check(!visible(pv,1), "camera update resets former visible room windows");
    pv.Update(id,outside,0);
    check(visible(pv,1), "moving camera back restores visible doorway");
    pv.Update(id,outside,42);
    check(visible(pv,1), "unknown camera sector fails open");
    pv.Reset();
    check(!pv.HasData(), "reset removes old world metadata");
    check(visible(pv,1), "reset avoids stale hidden-room state");
    P eyeCrossing=quad(0,1,-1.f,1.f,-.5f,.5f,.1f);
    eyeCrossing.normal={-.25f,0.f,1.f}; eyeCrossing.distance=.1f;
    eyeCrossing.vertices={{-1.f,-.5f,-.15f},{1.f,-.5f,.35f},{1.f,.5f,.35f},{-1.f,.5f,-.15f}};
    pv.Configure(2,{eyeCrossing}); pv.Update(proj,outside,0);
    check(visible(pv,1,-.1f,.1f,-.1f,.1f,2.f,2.1f), "portal crossing eye plane retains front region");
    pv.Configure(2,{quad(0,1,-.5f,.5f,-.5f,.5f,2.f)}); pv.Update(proj,outside,0);
    check(visible(pv,1,-.1f,.1f,-.1f,.1f,-1.f,2.f), "AABB crossing eye plane stays conservative");
    pv.Configure(2,{quad(1,0,-.3f,.3f,-.3f,.3f,.4f,false)}); pv.Update(id,V{0.f,0.f,.6f},1);
    check(!visible(pv,1,1.2f,1.5f), "wholly offscreen indoor AABB does not become viewport boundary intersection");
    const float nan=std::numeric_limits<float>::quiet_NaN();
    check(pv.IsVisible(1,V{nan,0.f,0.f},V{1.f,1.f,1.f}), "nonfinite object bounds fail open");
    check(pv.IsVisible(1,V{2.f,0.f,0.f},V{1.f,1.f,1.f}), "invalid object bound ordering fails open");
    M invalid=id; invalid.m[0][0]=nan; pv.Update(invalid,outside,0);
    check(visible(pv,1), "nonfinite frame matrix fails open");
    pv.Update(M{},outside,0);
    check(visible(pv,1), "uninitialized frame matrix fails open");
    pv.Update(id,V{nan,0.f,0.f},0);
    check(visible(pv,1), "nonfinite camera position fails open");
    P invalidPortal=quad(0,1,-.3f,.3f,-.3f,.3f,.4f); invalidPortal.vertices[0].x=nan;
    pv.Configure(2,{invalidPortal}); pv.Update(id,outside,0);
    check(!pv.HasData() && visible(pv,1), "incomplete portal metadata disables rejection");
    invalidPortal=quad(0,2,-.3f,.3f,-.3f,.3f,.4f);
    pv.Configure(2,{invalidPortal}); pv.Update(id,outside,0);
    check(!pv.HasData() && visible(pv,1), "invalid portal ownership disables rejection");

    std::mt19937 rng(81417);
    std::uniform_real_distribution<float> origin(-2.5f, 2.5f), extent(.08f, 1.2f), depth(.1f,150.f), sample(-.9f,.9f);
    for (int iteration = 0; iteration < 1000; ++iteration) {
        const float left=origin(rng), right=left+extent(rng), bottom=origin(rng), top=bottom+extent(rng), z=depth(rng);
        const float x=sample(rng), y=sample(rng);
        const float boxLeft=x-.025f, boxRight=x+.025f, boxBottom=y-.025f, boxTop=y+.025f;
        // Independent oracle for a parallel portal: perspective projected bounds
        // are exact x/z,y/z, then intersect with the canonical viewport.
        const float clippedLeft=std::max(left,-1.f), clippedRight=std::min(right,1.f);
        const float clippedBottom=std::max(bottom,-1.f), clippedTop=std::min(top,1.f);
        bool expected = z>0.000001f && clippedLeft<=clippedRight && clippedBottom<=clippedTop &&
                        boxLeft<=clippedRight && boxRight>=clippedLeft && boxBottom<=clippedTop && boxTop>=clippedBottom;
        const float margins[]={boxLeft-clippedRight,boxRight-clippedLeft,boxBottom-clippedTop,boxTop-clippedBottom,z-0.000001f};
        bool ambiguous=false;
        for(float margin:margins) if(std::abs(margin)<.001f) ambiguous=true;
        if(ambiguous) continue;
        pv.Configure(2,{quad(0,1,left*z,right*z,bottom*z,top*z,z)}); pv.Update(proj,outside,0);
        const bool actual=visible(pv,1,boxLeft*5.f,boxRight*5.f,boxBottom*5.f,boxTop*5.f,5.f,5.f);
        if(actual!=expected) {
            std::fprintf(stderr,"Oracle case %d portal=[%g,%g]x[%g,%g] z=%g box=[%g,%g]x[%g,%g] actual=%d expected=%d\n",iteration,left,right,bottom,top,z,boxLeft,boxRight,boxBottom,boxTop,actual,expected);
        }
        check(actual==expected,"random perspective portal visibility agrees with independent projection oracle");
    }
    std::printf("Portal visibility regression passed: %d assertions\n", checks);
}
