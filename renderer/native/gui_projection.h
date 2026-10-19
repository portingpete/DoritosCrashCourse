#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace Doritos::Graphics {
struct GuiProjectionVertex {
    std::array<float,4> clip;
    std::array<float,2> uv;
};
struct GuiProjectionHit {
    std::array<float,2> uv;
    float depth;
};

// Point coordinates are relative to the actual draw viewport, with the top
// left at (0,0). D3D depth is [0,1]. UVs remain in the material's texture space.
// A failed hit leaves the supplied result untouched.
inline bool projectedGuiTriangleHit(const std::array<GuiProjectionVertex,3>& triangle,
    const std::array<float,2>& point, GuiProjectionHit& hit) {
    for (const float coordinate : point)
        if (!std::isfinite(coordinate) || coordinate < 0 || coordinate > 1) return false;
    std::array<std::array<double,2>,3> screen{};
    std::array<double,3> inverseW{};
    for (size_t vertex = 0; vertex < triangle.size(); ++vertex) {
        for (const float coordinate : triangle[vertex].clip)
            if (!std::isfinite(coordinate)) return false;
        for (const float coordinate : triangle[vertex].uv)
            if (!std::isfinite(coordinate)) return false;
        if (triangle[vertex].clip[3] <= 0) return false;
        inverseW[vertex] = 1.0 / triangle[vertex].clip[3];
        screen[vertex] = {(triangle[vertex].clip[0] * inverseW[vertex] + 1) * .5,
            (1 - triangle[vertex].clip[1] * inverseW[vertex]) * .5};
    }
    const auto cross = [](const std::array<double,2>& a, const std::array<double,2>& b) {
        return a[0] * b[1] - a[1] * b[0];
    };
    const auto difference = [](const std::array<double,2>& a, const std::array<double,2>& b) {
        return std::array<double,2>{a[0] - b[0], a[1] - b[1]};
    };
    const auto ab = difference(screen[1],screen[0]), ac = difference(screen[2],screen[0]);
    const double area = cross(ab,ac);
    double largestEdgeSquared = 0;
    for (size_t vertex = 0; vertex < triangle.size(); ++vertex) {
        const auto edge = difference(screen[(vertex + 1) % 3],screen[vertex]);
        largestEdgeSquared = std::max(largestEdgeSquared,edge[0] * edge[0] + edge[1] * edge[1]);
    }
    constexpr double degeneracyTolerance = 64 * std::numeric_limits<double>::epsilon();
    if (!std::isfinite(area) || !std::isfinite(largestEdgeSquared) || !largestEdgeSquared ||
        std::abs(area) <= degeneracyTolerance * largestEdgeSquared) return false;
    const auto ap = difference({point[0],point[1]},screen[0]);
    const double b = cross(ap,ac) / area, c = cross(ab,ap) / area;
    std::array<double,3> weights{1 - b - c,b,c};
    double weightSum = 0;
    // Clip vertices and the normalized Win32 point arrive as floats. Allow
    // their rounding at tessellation seams without widening a pixel-sized gap.
    constexpr double edgeTolerance = 8 * std::numeric_limits<float>::epsilon();
    for (double& weight : weights) {
        if (!std::isfinite(weight) || weight < -edgeTolerance || weight > 1 + edgeTolerance) return false;
        weight = std::clamp(weight,0.0,1.0);
        weightSum += weight;
    }
    double depth = 0, divisor = 0;
    std::array<double,2> uv{};
    for (size_t vertex = 0; vertex < triangle.size(); ++vertex) {
        const double weight = weights[vertex] / weightSum;
        depth += weight * triangle[vertex].clip[2] * inverseW[vertex];
        const double perspectiveWeight = weight * inverseW[vertex];
        divisor += perspectiveWeight;
        for (size_t axis = 0; axis < uv.size(); ++axis)
            uv[axis] += perspectiveWeight * triangle[vertex].uv[axis];
    }
    if (!std::isfinite(depth) || depth < 0 || depth > 1 || !std::isfinite(divisor) || divisor <= 0) return false;
    const GuiProjectionHit candidate{{float(uv[0] / divisor),float(uv[1] / divisor)},float(depth)};
    if (!std::isfinite(candidate.uv[0]) || !std::isfinite(candidate.uv[1])) return false;
    hit = candidate;
    return true;
}

// Preserve the first hit on equal depth, and update only after a valid hit.
inline bool updateClosestProjectedGuiHit(const std::array<GuiProjectionVertex,3>& triangle,
    const std::array<float,2>& point, std::optional<GuiProjectionHit>& closest) {
    GuiProjectionHit candidate{};
    if (!projectedGuiTriangleHit(triangle,point,candidate) || (closest && candidate.depth >= closest->depth)) return false;
    closest = candidate;
    return true;
}
}
