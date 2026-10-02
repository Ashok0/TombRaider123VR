#pragma once
#include <string>

namespace tr::handskin {
// Visibility is independent of skinning. Zeroing a forearm palette entry
// collapses vertices with mixed wrist weights toward the render origin.
inline bool Patch(std::string& vertex, std::string& fragment) {
    const std::string anchor="p.z += dot(uJoints[index[2] + 2], coord) * weight;";
    const std::string main="void main()";
    const auto at=vertex.find(anchor);
    const auto fm=fragment.find(main);
    if (at==std::string::npos || vertex.find(main)==std::string::npos ||
        fm==std::string::npos || vertex.find("vec4 j = aLight;")==std::string::npos ||
        vertex.find("vec4 w = aColor;")==std::string::npos ||
        vertex.find("index[3]")!=std::string::npos) return false;
    const auto brace=fragment.find('{',fm);
    if (brace==std::string::npos) return false;
    vertex.insert(at+anchor.size(),
        "\n    ivec3 handIndex = ivec3(j.xyz);\n"
        "    int handJoint = int(uTrackedHand.x);\n"
        "    vTrackedHandWeight = (handIndex.x == handJoint ? w.x : 0.0)\n"
        "                       + (handIndex.y == handJoint ? w.y : 0.0)\n"
        "                       + (handIndex.z == handJoint ? w.z : 0.0);\n");
    vertex.insert(vertex.find(main),
        "uniform vec4 uTrackedHand;\nout float vTrackedHandWeight;\n");
    fragment.insert(brace+1,
        "\n    if (uTrackedHand.w > 0.5 && vTrackedHandWeight < 0.05) discard;\n");
    fragment.insert(fm,"uniform vec4 uTrackedHand;\nin float vTrackedHandWeight;\n");
    return true;
}
} // namespace tr::handskin
