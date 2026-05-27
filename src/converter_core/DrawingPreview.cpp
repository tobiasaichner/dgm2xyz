#include "dgm2xyz/DrawingPreview.h"

#include <algorithm>

namespace dgm2xyz {

bool Bounds2d::valid() const {
  return minX <= maxX && minY <= maxY;
}

double Bounds2d::width() const {
  return valid() ? maxX - minX : 0.0;
}

double Bounds2d::height() const {
  return valid() ? maxY - minY : 0.0;
}

void Bounds2d::include(double x, double y) {
  minX = std::min(minX, x);
  minY = std::min(minY, y);
  maxX = std::max(maxX, x);
  maxY = std::max(maxY, y);
}

void DrawingPreview::rebuildBounds() {
  bounds = Bounds2d{};

  for (const auto& point : points) {
    bounds.include(point.x, point.y);
  }

  for (const auto& line : lines) {
    bounds.include(line.x1, line.y1);
    bounds.include(line.x2, line.y2);
  }

  for (const auto& polyline : polylines) {
    for (const auto& vertex : polyline.vertices) {
      bounds.include(vertex.x, vertex.y);
    }
  }

  for (const auto& triangle : triangles) {
    bounds.include(triangle.a.x, triangle.a.y);
    bounds.include(triangle.b.x, triangle.b.y);
    bounds.include(triangle.c.x, triangle.c.y);
  }
}

bool DrawingPreview::hasErrors() const {
  return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
    return diagnostic.severity == DiagnosticSeverity::Error;
  });
}

ReadResult DrawingPreview::toReadResult() const {
  return ReadResult{points, diagnostics};
}

} // namespace dgm2xyz
