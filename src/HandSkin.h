#pragma once
#include <string>

namespace tr::handskin {
// Match the TR4/5 tracked-hand repair: skin the whole visible seam rigidly
// with the wrist, then trim the forearm side by its original hand weight.
inline bool Patch(std::string& vertex, std::string& fragment) {
    const std::string anchor="vec4 w = aColor;";
    const std::string main="void main()";
    const auto at=vertex.find(anchor);
    const auto fm=fragment.find(main);
    if (at==std::string::npos || vertex.find(main)==std::string::npos ||
        fm==std::string::npos || vertex.find("vec4 j = aLight;")==std::string::npos ||
        vertex.find("p.z += dot(uJoints[index[2] + 2], coord) * weight;")==std::string::npos ||
        vertex.find("index[3]")!=std::string::npos ||
        vertex.find("uTrackedHand")!=std::string::npos) return false;
    const auto brace=fragment.find('{',fm);
    if (brace==std::string::npos) return false;
    vertex.insert(at+anchor.size(),
        "\n    ivec3 handIndex = ivec3(j.xyz);\n"
        "    uint bodyBits = uint(uVisibleBody.x) | (uint(uVisibleBody.y) << 16u);\n"
        "    vVisibleBodyWeight = 0.0;\n"
        "    for (int b = 0; b < 3; ++b) {\n"
        "        if (handIndex[b] >= 0 && handIndex[b] < 32 &&\n"
        "            (bodyBits & (1u << uint(handIndex[b]))) != 0u)\n"
        "            vVisibleBodyWeight += w[b];\n"
        "    }\n"
        "    int handJoint = int(uTrackedHand.x);\n"
        "    vTrackedHandWeight = (handIndex.x == handJoint ? w.x : 0.0)\n"
        "                       + (handIndex.y == handJoint ? w.y : 0.0)\n"
        "                       + (handIndex.z == handJoint ? w.z : 0.0);\n"
        "    if (uTrackedHand.w > 0.5) {\n"
        "        j.xyz = vec3(float(handJoint));\n"
        "        w.xyz = vec3(1.0, 0.0, 0.0);\n"
        "    }\n");
    vertex.insert(vertex.find(main),
        "uniform vec4 uTrackedHand;\nout float vTrackedHandWeight;\n"
        "uniform vec4 uVisibleBody;\nout float vVisibleBodyWeight;\n");
    fragment.insert(brace+1,
        "\n    if (uTrackedHand.w > 0.5 && vTrackedHandWeight < 0.5) discard;\n"
        "    if (uVisibleBody.w > 0.5 && vVisibleBodyWeight < 0.5) discard;\n");
    fragment.insert(fm,"uniform vec4 uTrackedHand;\nin float vTrackedHandWeight;\n"
        "uniform vec4 uVisibleBody;\nin float vVisibleBodyWeight;\n");
    return true;
}
} // namespace tr::handskin
