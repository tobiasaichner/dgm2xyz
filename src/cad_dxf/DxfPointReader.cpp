#include "dgm2xyz/DxfPointReader.h"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <utility>

namespace dgm2xyz {
namespace {

struct DxfPair {
  int code = 0;
  std::string value;
};

struct EntityState {
  std::string type;
  std::optional<double> x;
  std::optional<double> y;
  std::optional<double> x2;
  std::optional<double> y2;
  std::optional<double> x3;
  std::optional<double> y3;
  std::optional<double> x4;
  std::optional<double> y4;
  double z = 0.0;
  double z2 = 0.0;
  double z3 = 0.0;
  double z4 = 0.0;
  double scaleX = 1.0;
  double scaleY = 1.0;
  double rotationDegrees = 0.0;
  std::string name;
  std::string layer;
  std::vector<Point2d> vertices;
  std::optional<double> pendingVertexX;
  bool closed = false;
};

std::string trim(std::string value) {
  const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char c) {
                return !isSpace(c);
              }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char c) {
                return !isSpace(c);
              }).base(),
              value.end());
  return value;
}

bool containsNulByte(const std::string& content) {
  return content.find('\0') != std::string::npos;
}

std::optional<double> parseDouble(const std::string& value) {
  const auto trimmed = trim(value);
  double parsed = 0.0;
  const auto* begin = trimmed.data();
  const auto* end = trimmed.data() + trimmed.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec == std::errc{} && result.ptr == end) {
    return parsed;
  }
  return std::nullopt;
}

std::optional<int> parseCode(const std::string& value) {
  const auto trimmed = trim(value);
  int parsed = 0;
  const auto* begin = trimmed.data();
  const auto* end = trimmed.data() + trimmed.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec == std::errc{} && result.ptr == end) {
    return parsed;
  }
  return std::nullopt;
}

std::vector<DxfPair> parsePairs(const std::string& content, std::vector<Diagnostic>& diagnostics) {
  std::vector<DxfPair> pairs;
  std::istringstream stream(content);
  std::string codeLine;
  std::string valueLine;
  int line = 1;

  while (std::getline(stream, codeLine)) {
    if (!std::getline(stream, valueLine)) {
      diagnostics.push_back({DiagnosticSeverity::Error, "Malformed DXF: dangling group code at end of file."});
      break;
    }

    if (!valueLine.empty() && valueLine.back() == '\r') {
      valueLine.pop_back();
    }
    if (!codeLine.empty() && codeLine.back() == '\r') {
      codeLine.pop_back();
    }

    const auto code = parseCode(codeLine);
    if (!code) {
      diagnostics.push_back({DiagnosticSeverity::Error, "Malformed DXF: invalid group code near line " + std::to_string(line) + "."});
      break;
    }

    pairs.push_back(DxfPair{*code, trim(valueLine)});
    line += 2;
  }

  return pairs;
}

bool isEntityType(const std::string& value) {
  return value == "POINT" || value == "INSERT" || value == "LINE" || value == "CIRCLE" ||
         value == "TEXT" || value == "MTEXT" || value == "POLYLINE" || value == "LWPOLYLINE" ||
         value == "VERTEX" || value == "SEQEND" || value == "ARC" || value == "SPLINE" ||
         value == "3DFACE" || value == "BLOCK" || value == "ENDBLK" || value == "ENDSEC" || value == "EOF";
}

bool isSupportedPreviewEntity(const std::string& value) {
  return value == "POINT" || value == "INSERT" || value == "LINE" || value == "LWPOLYLINE" || value == "POLYLINE" ||
         value == "3DFACE";
}

bool isAllowedInsert(const std::vector<std::string>& allowedNames, const std::string& name) {
  if (allowedNames.empty()) {
    return true;
  }

  return std::find(allowedNames.begin(), allowedNames.end(), name) != allowedNames.end();
}

bool startsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), value.begin());
}

bool isDgmBlock(const std::string& blockName) {
  return startsWith(blockName, "FIGX");
}

bool isGelaendemodellLayer(const std::string& layer) {
  return layer.find("Geländemodell") != std::string::npos ||
         layer.find("Gelaendemodell") != std::string::npos;
}

std::string insertSource(const EntityState& entity) {
  const auto blockName = entity.name.empty() ? "(unnamed block)" : entity.name;
  if (isDgmBlock(blockName) || isGelaendemodellLayer(entity.layer)) {
    return "Geländemodell points";
  }

  if (!entity.layer.empty()) {
    return "Layer: " + entity.layer;
  }

  return "Block reference | block=" + blockName;
}

std::string pointSource(const EntityState& entity) {
  if (!entity.layer.empty()) {
    return "POINT [" + entity.layer + "]";
  }
  return "POINT";
}

bool distinctPoint(Point2d left, Point2d right) {
  return std::abs(left.x - right.x) > 0.000001 || std::abs(left.y - right.y) > 0.000001;
}

Point2d transformPoint(Point2d point, const EntityState& insert) {
  const double angle = insert.rotationDegrees * 3.14159265358979323846 / 180.0;
  const double scaledX = point.x * insert.scaleX;
  const double scaledY = point.y * insert.scaleY;
  const double rotatedX = scaledX * std::cos(angle) - scaledY * std::sin(angle);
  const double rotatedY = scaledX * std::sin(angle) + scaledY * std::cos(angle);
  return Point2d{insert.x.value_or(0.0) + rotatedX, insert.y.value_or(0.0) + rotatedY};
}

std::string dgmSource(const std::string& block, const std::string& layer) {
  if (!block.empty() && !layer.empty()) {
    return "DGM | block=" + block + " | layer=" + layer;
  }
  if (!block.empty()) {
    return "DGM | block=" + block;
  }
  if (!layer.empty()) {
    return "DGM | layer=" + layer;
  }
  return "DGM";
}

void appendFaceTriangles(PreviewVertex p1,
                         PreviewVertex p2,
                         PreviewVertex p3,
                         PreviewVertex p4,
                         std::string layer,
                         std::string source,
                         std::string block,
                         DrawingPreview& preview) {
  preview.triangles.push_back(PreviewTriangle{p1, p2, p3, layer, source, block});
  if (distinctPoint(Point2d{p3.x, p3.y}, Point2d{p4.x, p4.y})) {
    preview.triangles.push_back(PreviewTriangle{p1, p3, p4, std::move(layer), std::move(source), std::move(block)});
  }
}

void appendTransformedBlockFaces(const EntityState& insert,
                                 const std::map<std::string, std::vector<PreviewTriangle>>& blockFaces,
                                 DrawingPreview& preview) {
  const auto found = blockFaces.find(insert.name);
  if (found == blockFaces.end()) {
    return;
  }

  for (const auto& triangle : found->second) {
    const auto source = dgmSource(insert.name, insert.layer);
    preview.triangles.push_back(PreviewTriangle{
        PreviewVertex{transformPoint(Point2d{triangle.a.x, triangle.a.y}, insert).x, transformPoint(Point2d{triangle.a.x, triangle.a.y}, insert).y, triangle.a.z + insert.z},
        PreviewVertex{transformPoint(Point2d{triangle.b.x, triangle.b.y}, insert).x, transformPoint(Point2d{triangle.b.x, triangle.b.y}, insert).y, triangle.b.z + insert.z},
        PreviewVertex{transformPoint(Point2d{triangle.c.x, triangle.c.y}, insert).x, transformPoint(Point2d{triangle.c.x, triangle.c.y}, insert).y, triangle.c.z + insert.z},
        insert.layer.empty() ? triangle.layer : insert.layer,
        source,
        insert.name,
    });
  }
}

void finishEntity(const EntityState& entity,
                  const std::vector<std::string>& allowedNames,
                  DrawingPreview& preview,
                  const std::map<std::string, std::vector<PreviewTriangle>>& blockFaces = {}) {
  if (entity.type != "POINT" && entity.type != "INSERT" && entity.type != "LINE" && entity.type != "LWPOLYLINE" &&
      entity.type != "3DFACE") {
    return;
  }

  if (entity.type == "INSERT" && !isAllowedInsert(allowedNames, entity.name)) {
    return;
  }

  if (entity.type == "INSERT") {
    appendTransformedBlockFaces(entity, blockFaces, preview);
  }

  if (entity.type == "LINE") {
    if (entity.x && entity.y && entity.x2 && entity.y2) {
      preview.lines.push_back(PreviewLine{*entity.x, *entity.y, *entity.x2, *entity.y2, entity.layer});
    }
    return;
  }

  if (entity.type == "LWPOLYLINE") {
    if (entity.vertices.size() >= 2) {
      preview.polylines.push_back(PreviewPolyline{entity.vertices, entity.closed, entity.layer});
    }
    return;
  }

  if (entity.type == "3DFACE") {
    if (entity.x && entity.y && entity.x2 && entity.y2 && entity.x3 && entity.y3) {
      const PreviewVertex p1{*entity.x, *entity.y, entity.z};
      const PreviewVertex p2{*entity.x2, *entity.y2, entity.z2};
      const PreviewVertex p3{*entity.x3, *entity.y3, entity.z3};
      const PreviewVertex p4{entity.x4.value_or(*entity.x3), entity.y4.value_or(*entity.y3), entity.x4 && entity.y4 ? entity.z4 : entity.z3};
      appendFaceTriangles(p1, p2, p3, p4, entity.layer, dgmSource("", entity.layer), "", preview);
    }
    return;
  }

  if (!entity.x || !entity.y) {
    preview.diagnostics.push_back({DiagnosticSeverity::Warning, "Skipped " + entity.type + " entity with missing X or Y coordinate."});
    return;
  }

  const auto source = entity.type == "INSERT" ? insertSource(entity) : pointSource(entity);
  preview.points.push_back(Point{*entity.x, *entity.y, entity.z, source});
}

void promoteLargestBlockGroupToDgm(DrawingPreview& preview) {
  std::map<std::string, std::size_t> counts;
  bool hasDgmGroup = false;
  for (const auto& point : preview.points) {
    if (point.source == "Geländemodell points") {
      hasDgmGroup = true;
    }
    if (startsWith(point.source, "Layer: ") || startsWith(point.source, "Block reference | ")) {
      counts[point.source]++;
    }
  }

  if (hasDgmGroup || counts.empty()) {
    return;
  }

  const auto largest = std::max_element(counts.begin(), counts.end(), [](const auto& left, const auto& right) {
    return left.second < right.second;
  });

  for (auto& point : preview.points) {
    if (point.source == largest->first) {
      point.source = "Geländemodell points";
    }
  }

  preview.diagnostics.push_back({DiagnosticSeverity::Info, "Classified largest block point group as Geländemodell points."});
}

} // namespace

DxfPointReader::DxfPointReader(std::vector<std::string> allowedInsertBlockNames)
    : allowedInsertBlockNames_(std::move(allowedInsertBlockNames)) {}

ReadResult DxfPointReader::readPoints(const std::filesystem::path& file) const {
  return readPreview(file).toReadResult();
}

DrawingPreview DxfPointReader::readPreview(const std::filesystem::path& file) const {
  DrawingPreview preview;

  std::ifstream input(file, std::ios::binary);
  if (!input) {
    preview.diagnostics.push_back({DiagnosticSeverity::Error, "Could not open input file."});
    return preview;
  }

  const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (containsNulByte(content)) {
    preview.diagnostics.push_back({DiagnosticSeverity::Error, "Binary DXF is not supported by the initial reader."});
    return preview;
  }

  auto pairs = parsePairs(content, preview.diagnostics);
  if (preview.hasErrors()) {
    return preview;
  }

  EntityState current;
  bool inEntity = false;
  bool inEntitiesSection = false;
  bool inBlocksSection = false;
  bool awaitingSectionName = false;
  bool inPolyline = false;
  std::string currentBlockName;
  PreviewPolyline polyline;
  std::map<std::string, std::vector<PreviewTriangle>> blockFaces;
  std::map<std::string, std::size_t> ignoredEntities;

  const auto finishCurrent = [&] {
    if (!inEntity) {
      return;
    }

    if (inBlocksSection && !currentBlockName.empty()) {
      DrawingPreview blockPreview;
      finishEntity(current, allowedInsertBlockNames_, blockPreview);
      auto& faces = blockFaces[currentBlockName];
      faces.insert(faces.end(), blockPreview.triangles.begin(), blockPreview.triangles.end());
    } else {
      finishEntity(current, allowedInsertBlockNames_, preview, blockFaces);
    }
  };

  for (const auto& pair : pairs) {
    if (pair.code == 0 && pair.value == "SECTION") {
      awaitingSectionName = true;
      inEntitiesSection = false;
      inBlocksSection = false;
      continue;
    }

    if (awaitingSectionName && pair.code == 2) {
      inEntitiesSection = pair.value == "ENTITIES";
      inBlocksSection = pair.value == "BLOCKS";
      awaitingSectionName = false;
      continue;
    }

    if (!inEntitiesSection && !inBlocksSection) {
      continue;
    }

    if (pair.code == 0 && isEntityType(pair.value)) {
      if (inBlocksSection && pair.value == "BLOCK") {
        currentBlockName.clear();
        current = EntityState{};
        current.type = "BLOCK";
        inEntity = false;
        continue;
      }

      if (inBlocksSection && pair.value == "ENDBLK") {
        finishCurrent();
        current = EntityState{};
        inEntity = false;
        currentBlockName.clear();
        continue;
      }

      if (inPolyline && pair.value == "VERTEX") {
        current = EntityState{};
        current.type = "VERTEX";
        inEntity = true;
        continue;
      }

      if (inPolyline && pair.value == "SEQEND") {
        if (polyline.vertices.size() >= 2) {
          preview.polylines.push_back(polyline);
        }
        polyline = PreviewPolyline{};
        inPolyline = false;
        current = EntityState{};
        inEntity = false;
        continue;
      }

      if (inEntity) {
        finishCurrent();
      }

      current = EntityState{};
      current.type = pair.value;
      inEntity = isSupportedPreviewEntity(pair.value);
      if (!inEntity && pair.value != "ENDSEC" && pair.value != "EOF") {
        ignoredEntities[pair.value]++;
      }
      if (pair.value == "POLYLINE") {
        inPolyline = true;
        polyline = PreviewPolyline{};
      }
      continue;
    }

    if (!inEntity) {
      if (inBlocksSection && current.type == "BLOCK" && pair.code == 2) {
        currentBlockName = pair.value;
      }
      continue;
    }

    if (pair.code == 2 && current.type == "INSERT") {
      current.name = pair.value;
      continue;
    }

    if (pair.code == 8) {
      current.layer = pair.value;
      if (inPolyline && current.type == "POLYLINE") {
        polyline.layer = pair.value;
      }
      continue;
    }

    if (pair.code == 70 && (current.type == "LWPOLYLINE" || current.type == "POLYLINE")) {
      const auto flag = parseCode(pair.value);
      if (flag) {
        current.closed = ((*flag & 1) != 0);
        if (current.type == "POLYLINE") {
          polyline.closed = current.closed;
        }
      }
      continue;
    }

    if (current.type == "INSERT") {
      if (pair.code == 41 || pair.code == 42 || pair.code == 50) {
        const auto value = parseDouble(pair.value);
        if (!value) {
          preview.diagnostics.push_back({DiagnosticSeverity::Warning, "Skipped invalid numeric INSERT transform value."});
          continue;
        }
        if (pair.code == 41) {
          current.scaleX = *value;
        } else if (pair.code == 42) {
          current.scaleY = *value;
        } else {
          current.rotationDegrees = *value;
        }
        continue;
      }
    }

    if (pair.code == 10 || pair.code == 20 || pair.code == 30 || pair.code == 11 || pair.code == 21 ||
        pair.code == 31 || pair.code == 12 || pair.code == 22 || pair.code == 32 || pair.code == 13 ||
        pair.code == 23 || pair.code == 33) {
      const auto value = parseDouble(pair.value);
      if (!value) {
        preview.diagnostics.push_back({DiagnosticSeverity::Warning, "Skipped invalid numeric coordinate value."});
        continue;
      }

      if (current.type == "VERTEX") {
        if (pair.code == 10) {
          current.x = *value;
        } else if (pair.code == 20) {
          current.y = *value;
          if (current.x) {
            polyline.vertices.push_back(Point2d{*current.x, *current.y});
          }
        }
        continue;
      }

      if (current.type == "LWPOLYLINE") {
        if (pair.code == 10) {
          current.pendingVertexX = *value;
        } else if (pair.code == 20 && current.pendingVertexX) {
          current.vertices.push_back(Point2d{*current.pendingVertexX, *value});
          current.pendingVertexX.reset();
        } else if (pair.code == 30) {
          current.z = *value;
        }
        continue;
      }

      if (pair.code == 10) {
        current.x = *value;
      } else if (pair.code == 20) {
        current.y = *value;
      } else if (pair.code == 11) {
        current.x2 = *value;
      } else if (pair.code == 21) {
        current.y2 = *value;
      } else if (pair.code == 31) {
        current.z2 = *value;
      } else if (pair.code == 12) {
        current.x3 = *value;
      } else if (pair.code == 22) {
        current.y3 = *value;
      } else if (pair.code == 32) {
        current.z3 = *value;
      } else if (pair.code == 13) {
        current.x4 = *value;
      } else if (pair.code == 23) {
        current.y4 = *value;
      } else if (pair.code == 33) {
        current.z4 = *value;
      } else {
        current.z = *value;
      }
    }
  }

  if (inEntity) {
    finishCurrent();
  }

  promoteLargestBlockGroupToDgm(preview);
  preview.rebuildBounds();
  preview.diagnostics.push_back({DiagnosticSeverity::Info, "Preview background: " + std::to_string(preview.lines.size()) + " line(s), " + std::to_string(preview.polylines.size()) + " polyline(s), " + std::to_string(preview.triangles.size()) + " triangle(s)."});
  if (!ignoredEntities.empty()) {
    std::ostringstream stream;
    stream << "Ignored DXF preview entities:";
    for (const auto& [type, count] : ignoredEntities) {
      stream << ' ' << type << '=' << count;
    }
    preview.diagnostics.push_back({DiagnosticSeverity::Info, stream.str()});
  }
  return preview;
}

} // namespace dgm2xyz
