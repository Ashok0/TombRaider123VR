#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace tr::actionicon {
struct Point { int32_t x, y, z; };

// Keep a native-approved nearby prompt inside the flat HUD's visible panel.
// TR1-3's w2v rotation is 14-bit fixed point, unlike TR4/5's float matrix.
inline bool Place(Point& point, const int32_t matrix[12], float perspective,
                  float centerX, float centerY, float nearZ, float farZ) {
    if (!std::isfinite(perspective) || perspective <= 0 ||
        !std::isfinite(centerX) || !std::isfinite(centerY) ||
        centerX <= 0 || centerY <= 0 ||
        !std::isfinite(nearZ) || !std::isfinite(farZ) ||
        nearZ < 0 || farZ <= nearZ + 64)
        return false;
    double rotation[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            rotation[i][j] = double(matrix[i * 4 + j]) / 16384.0;
    for (int row = 0; row < 3; ++row)
        for (int other = 0; other <= row; ++other) {
            double dot = 0;
            for (int j = 0; j < 3; ++j)
                dot += rotation[row][j] * rotation[other][j];
            if (std::fabs(dot - (row == other ? 1.0 : 0.0)) > 0.002)
                return false;
        }

    const double delta[3] = {
        double(point.x) - matrix[3],
        double(point.y) - matrix[7],
        double(point.z) - matrix[11]
    };
    if (delta[0]*delta[0] + delta[1]*delta[1] + delta[2]*delta[2] > 1024.0*1024.0)
        return false;
    double view[3]{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            view[i] += rotation[i][j] * delta[j];
    if (view[2] < -256 || view[2] >= farZ) return false;
    const double depth = std::max(256.0, double(nearZ) + 32.0);
    if (depth >= farZ) return false;
    const double denominator = std::max(depth, view[2]);
    const double maxX = 0.70 * centerX / perspective;
    const double maxY = 0.70 * centerY / perspective;
    const double x = std::clamp(view[0] / denominator, -maxX, maxX);
    const double y = std::clamp(view[1] / denominator, -maxY, maxY);
    if (view[2] >= depth &&
        x == view[0] / denominator && y == view[1] / denominator)
        return false;
    const double adjusted[3] = {x*denominator, y*denominator, denominator};
    int32_t result[3];
    for (int j = 0; j < 3; ++j) {
        double world = matrix[j*4+3];
        for (int i = 0; i < 3; ++i)
            world += rotation[i][j] * adjusted[i];
        if (!std::isfinite(world) ||
            world < double(std::numeric_limits<int32_t>::min())+1 ||
            world > double(std::numeric_limits<int32_t>::max())-1)
            return false;
        result[j] = static_cast<int32_t>(std::lround(world));
    }
    point = {result[0], result[1], result[2]};
    return true;
}
} // namespace tr::actionicon
