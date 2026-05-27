#pragma once

#include "dgm2xyz/CadPointReader.h"

#include <limits>
#include <string>
#include <vector>

namespace dgm2xyz {

struct Point2d {
  double x = 0.0;
  double y = 0.0;
};

struct PreviewVertex {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct Bounds2d {
  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();

  [[nodiscard]] bool valid() const;
  [[nodiscard]] double width() const;
  [[nodiscard]] double height() const;
  void include(double x, double y);
};

struct PreviewLine {
  double x1 = 0.0;
  double y1 = 0.0;
  double x2 = 0.0;
  double y2 = 0.0;
  std::string layer;
};

struct PreviewPolyline {
  std::vector<Point2d> vertices;
  bool closed = false;
  std::string layer;
};

struct PreviewTriangle {
  PreviewVertex a;
  PreviewVertex b;
  PreviewVertex c;
  std::string layer;
  std::string source;
  std::string block;
};

struct DrawingPreview {
  std::vector<Point> points;
  std::vector<PreviewLine> lines;
  std::vector<PreviewPolyline> polylines;
  std::vector<PreviewTriangle> triangles;
  std::vector<Diagnostic> diagnostics;
  Bounds2d bounds;

  void rebuildBounds();
  [[nodiscard]] bool hasErrors() const;
  [[nodiscard]] ReadResult toReadResult() const;
};

} // namespace dgm2xyz
