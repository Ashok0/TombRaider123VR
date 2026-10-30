#pragma once
#include <string>

namespace tr::motionhandskin {
// Native MaskJoints zeroes the hidden forearm matrix. Blending against it
// shrinks wrist vertices toward the render origin (p.w remains 1). For the
// scoped tracked-hand draw, use the existing corrected wrist palette rigidly
// and trim the hidden side of the seam in the fragment shader instead.
inline bool Patch(std::string& vertex,std::string& fragment) {
    const char* anchor="vec4 w = aColor;";
    const auto at=vertex.find(anchor),main=vertex.find("void main()"),fm=fragment.find("void main()");
    if (at==std::string::npos || main==std::string::npos || fm==std::string::npos ||
        vertex.find(anchor,at+1)!=std::string::npos ||
        vertex.find("vec4 j = aLight;")==std::string::npos ||
        vertex.find("p.z += dot(uJoints[index[2] + 2], coord) * weight;")==std::string::npos ||
        vertex.find("uTrackedHandJoint")!=std::string::npos ||
        fragment.find("vTrackedHandWeight")!=std::string::npos) return false;
    const auto brace=fragment.find('{',fm);
    if (brace==std::string::npos) return false;
    vertex.insert(at+std::char_traits<char>::length(anchor),
        "\n    ivec3 bodyIndices = ivec3(j.xyz);\n"
        "    uint bodyBits = uint(uVisibleBody.x) | (uint(uVisibleBody.y) << 16u);\n"
        "    vVisibleBodyWeight = 0.0;\n"
        "    vBodyWristWeight = 0.0;\n"
        "    uint wristBits = uint(uBodyWristSeams.x) | (uint(uBodyWristSeams.y) << 16u);\n"
        "    for (int b=0;b<3;++b) {\n"
        "        int joint=bodyIndices[b];\n"
        "        if (joint>=0 && joint<33 && ((joint<32 ? bodyBits : uint(uVisibleBody.z)) & (1u << uint(joint % 32)))!=0u)\n"
        "            vVisibleBodyWeight += w[b];\n"
        "        if (joint>=0 && joint<33 && ((joint<32 ? wristBits : uint(uBodyWristSeams.z)) & (1u << uint(joint % 32)))!=0u)\n"
        "            vBodyWristWeight += w[b];\n"
        "    }\n"
        "    vTrackedHandWeight = 1.0;\n"
        "    if (uTrackedHandJoint > 0) {\n"
        "        int handJoint = uTrackedHandJoint - 1;\n"
        "        ivec3 handIndices = ivec3(j.xyz);\n"
        "        vTrackedHandWeight = 0.0;\n"
        "        for (int k=0;k<3;++k) if (handIndices[k] == handJoint) vTrackedHandWeight += w[k];\n"
        "        j.xyz = vec3(float(handJoint));\n"
        "        w.xyz = vec3(1.0,0.0,0.0);\n"
        "    }\n");
    vertex.insert(main,"uniform int uTrackedHandJoint;\nout float vTrackedHandWeight;\nuniform vec4 uVisibleBody;\nout float vVisibleBodyWeight;\nuniform vec4 uBodyWristSeams;\nout float vBodyWristWeight;\n");
    // Costume cuffs can be mostly wrist-weighted. Keep their full blended
    // surface wherever it still has visible forearm/body support. Pure hidden
    // hand surfaces stay excluded, and head/torso masking keeps its threshold.
    fragment.insert(brace+1,"\n    if (vTrackedHandWeight < 0.5) discard;\n    if (uVisibleBody.w > 0.5 && vVisibleBodyWeight < 0.5 && !(vBodyWristWeight > 0.0 && vVisibleBodyWeight > 0.0)) discard;\n");
    fragment.insert(fm,"in float vTrackedHandWeight;\nuniform vec4 uVisibleBody;\nin float vVisibleBodyWeight;\nin float vBodyWristWeight;\n");
    return true;
}
} // namespace tr::motionhandskin
