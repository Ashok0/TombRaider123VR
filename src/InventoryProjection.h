#pragma once
#include "StereoMath.h"

namespace tr::inventory {
// One camera-space frame for the native screen layout: both the text and the
// ring use the same panel width, height, anchor and eye transform. Keep the
// native perspective's x/y rows (including its off-axis offsets), rather than
// replacing its field of view with the HMD's and separating items from labels.
inline mat4 MenuToAnchor(const mat4& native, bool perspective, bool mesh,
                         float depth, float width, float height, float slab) {
    if(perspective) {
        // Map a nominal front-item distance to the label plane. This is a
        // spatial scale choice, not a clipping distance: all native x/w and
        // y/w positions survive, at every depth, and meshes keep real volume.
        constexpr float referenceDepth=640.0f;
        mat4 out{};
        for(int c=0;c<4;++c) {
            out.m[c*4]   = native.m[c*4]   * width/referenceDepth;
            out.m[c*4+1] = native.m[c*4+1] * height/referenceDepth;
            out.m[c*4+2] = -native.m[c*4+3]* depth/referenceDepth;
        }
        out.m[15]=1;
        return out;
    }
    mat4 lift{};
    lift.m[0]=width;lift.m[5]=height;
    lift.m[10]=mesh ? -slab : 0;
    lift.m[14]=-depth;lift.m[15]=1;
    return Mul4(lift,native);
}
}
