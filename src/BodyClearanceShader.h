#pragma once
#include <string>

namespace tr::bodyclearance {
// Trim only body-weighted fragments inside the shared headset volume. This
// never translates the camera/body or clips tracked arms, weapons and hands.
inline bool Patch(std::string& vertex,std::string& fragment) {
    const auto projection=vertex.find("gl_Position ="),vm=vertex.find("void main()"),fm=fragment.find("void main()");
    if(projection==std::string::npos || vm==std::string::npos || fm==std::string::npos ||
       vertex.find("vec4 j = aLight;")==std::string::npos ||
       vertex.find("p.z += dot(uJoints[index[2] + 2], coord) * weight;")==std::string::npos ||
       vertex.find("index[3]")!=std::string::npos || vertex.find("uBodyClearance")!=std::string::npos || fragment.find("uBodyClearance")!=std::string::npos) return false;
    const auto brace=fragment.find('{',fm);
    if(brace==std::string::npos) return false;
    vertex.insert(projection,
        "vBodyClearanceOffset = p.xyz - uBodyClearance.xyz;\n"
        "    vBodyClearanceWeight = 0.0;\n"
        "    uint clearanceBits = uint(uBodyClearanceJoints.x) | (uint(uBodyClearanceJoints.y) << 16u);\n"
        "    for (int k=0;k<3;++k) {\n"
        "        int joint=int(aLight[k]);\n"
        "        if (joint>=0 && joint<33 && ((joint<32 ? clearanceBits : uint(uBodyClearanceJoints.z)) & (1u << uint(joint % 32)))!=0u)\n"
        "            vBodyClearanceWeight += aColor[k];\n"
        "    }\n    ");
    vertex.insert(vm,"uniform vec4 uBodyClearance;\nuniform vec4 uBodyClearanceJoints;\nout vec3 vBodyClearanceOffset;\nout float vBodyClearanceWeight;\n");
    fragment.insert(brace+1,"\n    if (uBodyClearance.w > 0.0 && vBodyClearanceWeight > 0.5 && dot(vBodyClearanceOffset,vBodyClearanceOffset) < uBodyClearance.w*uBodyClearance.w) discard;\n");
    fragment.insert(fm,"uniform vec4 uBodyClearance;\nin vec3 vBodyClearanceOffset;\nin float vBodyClearanceWeight;\n");
    return true;
}
}
