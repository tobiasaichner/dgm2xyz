#include "dgm2xyz/DwgPointReader.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

#ifdef DGM2XYZ_WITH_LIBREDWG
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libredwg/dwg.h>
}
#endif

namespace dgm2xyz {
namespace {

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

std::string insertSource(const std::string& blockName, const std::string& layer) {
  const auto displayName = blockName.empty() ? "(unnamed block)" : blockName;
  if (isDgmBlock(displayName) || isGelaendemodellLayer(layer)) {
    return "Geländemodell points";
  }

  if (!layer.empty()) {
    return "Layer: " + layer;
  }

  return "Block reference | block=" + displayName;
}

std::string pointSource(const std::string& layer) {
  if (!layer.empty()) {
    return "POINT [" + layer + "]";
  }
  return "POINT";
}

std::string pathToUtf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return std::string(value.begin(), value.end());
}

#ifdef DGM2XYZ_WITH_LIBREDWG
struct InsertGroupDebug {
  std::size_t count = 0;
  std::string source;
  std::string layer;
  std::string resolvedBlock;
  std::string rawBlockName;
  double sampleX = 0.0;
  double sampleY = 0.0;
  double sampleZ = 0.0;
  double scaleX = 0.0;
  double scaleY = 0.0;
  double scaleZ = 0.0;
  double rotation = 0.0;
  bool hasAttribs = false;
  BITCODE_BL numOwned = 0;
  std::string attributes;
};

std::string textOrEmpty(const char* value) {
  return value ? value : "";
}

std::string formatDouble(double value) {
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  stream << std::fixed << std::setprecision(4) << value;
  return stream.str();
}

std::string entityLayerName(const Dwg_Object_Entity* entity) {
  if (!entity) {
    return {};
  }

  const auto* layer = dwg_get_entity_layer(entity);
  return layer ? textOrEmpty(layer->name) : "";
}

struct InsertTransform {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double scaleX = 1.0;
  double scaleY = 1.0;
  double scaleZ = 1.0;
  double rotation = 0.0;
};

Point2d transformPoint(Point2d point, const InsertTransform& transform) {
  const double scaledX = point.x * transform.scaleX;
  const double scaledY = point.y * transform.scaleY;
  const double rotatedX = scaledX * std::cos(transform.rotation) - scaledY * std::sin(transform.rotation);
  const double rotatedY = scaledX * std::sin(transform.rotation) + scaledY * std::cos(transform.rotation);
  return Point2d{transform.x + rotatedX, transform.y + rotatedY};
}

bool distinctPoint(Point2d left, Point2d right) {
  return std::abs(left.x - right.x) > 0.000001 || std::abs(left.y - right.y) > 0.000001;
}

bool distinctVertex(PreviewVertex left, PreviewVertex right) {
  return distinctPoint(Point2d{left.x, left.y}, Point2d{right.x, right.y}) || std::abs(left.z - right.z) > 0.000001;
}

std::string dgmSource(const std::string& block, const std::string& layer, const std::string& identity = {}) {
  std::string result = "DGM";
  if (!block.empty() && !layer.empty()) {
    result += " | block=" + block;
    if (!identity.empty()) {
      result += " | id=" + identity;
    }
    result += " | layer=" + layer;
    return result;
  }
  if (!block.empty()) {
    result += " | block=" + block;
    if (!identity.empty()) {
      result += " | id=" + identity;
    }
    return result;
  }
  if (!identity.empty()) {
    result += " | id=" + identity;
  }
  if (!layer.empty()) {
    result += " | layer=" + layer;
  }
  return result;
}

PreviewVertex transformVertex(PreviewVertex vertex, const InsertTransform& transform) {
  const auto point = transformPoint(Point2d{vertex.x, vertex.y}, transform);
  return PreviewVertex{point.x, point.y, transform.z + vertex.z * transform.scaleZ};
}

void appendTrianglePair(PreviewVertex p1,
                        PreviewVertex p2,
                        PreviewVertex p3,
                        PreviewVertex p4,
                        std::string layer,
                        std::string source,
                        std::string block,
                        DrawingPreview& preview) {
  preview.triangles.push_back(PreviewTriangle{p1, p2, p3, layer, source, block});
  if (distinctVertex(p3, p4)) {
    preview.triangles.push_back(PreviewTriangle{p1, p3, p4, std::move(layer), std::move(source), std::move(block)});
  }
}

std::string insertBlockName(Dwg_Data& dwg, const Dwg_Entity_INSERT* insert) {
  if (!insert) {
    return {};
  }

  const std::string rawBlockName = textOrEmpty(insert->block_name);
  if (!rawBlockName.empty() && rawBlockName != "*") {
    return rawBlockName;
  }

  if (insert->block_header) {
    char* resolvedName = dwg_handle_name(&dwg, "BLOCK", insert->block_header);
    if (resolvedName && resolvedName[0] != '\0') {
      std::string result = resolvedName;
      std::free(resolvedName);
      return result;
    }
    std::free(resolvedName);
  }

  if (!rawBlockName.empty()) {
    return rawBlockName;
  }

  const auto* blockObject = dwg_ref_object(&dwg, insert->block_header);
  if (!blockObject || !blockObject->tio.object || !blockObject->tio.object->tio.BLOCK_HEADER) {
    return {};
  }

  return textOrEmpty(blockObject->tio.object->tio.BLOCK_HEADER->name);
}

std::string handleIdentity(BITCODE_H handle) {
  if (!handle || handle->absolute_ref == 0) {
    return {};
  }

  std::ostringstream stream;
  stream << std::uppercase << std::hex << handle->absolute_ref;
  return stream.str();
}

std::string blockDisplayName(std::string blockName, const std::string& identity) {
  if (blockName.empty()) {
    return identity.empty() ? blockName : "block#" + identity;
  }
  if (blockName == "*" && !identity.empty()) {
    return blockName + "#" + identity;
  }
  return blockName;
}

std::string insertAttributes(Dwg_Data& dwg, const Dwg_Entity_INSERT* insert) {
  if (!insert || !insert->attribs || insert->num_owned == 0) {
    return "";
  }

  std::ostringstream stream;
  bool first = true;
  for (BITCODE_BL index = 0; index < insert->num_owned; ++index) {
    const auto* attributeObject = dwg_ref_object(&dwg, insert->attribs[index]);
    if (!attributeObject || !attributeObject->tio.entity || !attributeObject->tio.entity->tio.ATTRIB) {
      continue;
    }

    const auto* attribute = attributeObject->tio.entity->tio.ATTRIB;
    if (!first) {
      stream << "; ";
    }

    const auto tag = textOrEmpty(attribute->tag);
    const auto value = textOrEmpty(attribute->text_value);
    stream << (tag.empty() ? "(untagged)" : tag) << '=' << value;
    first = false;
  }

  return stream.str();
}

void rememberInsertGroup(std::map<std::string, InsertGroupDebug>& groups,
                         Dwg_Data& dwg,
                         const Dwg_Entity_INSERT* insert,
                         std::string source,
                         std::string layer,
                         std::string resolvedBlock) {
  auto& group = groups[source];
  group.count++;
  if (group.count > 1) {
    return;
  }

  group.source = std::move(source);
  group.layer = std::move(layer);
  group.resolvedBlock = std::move(resolvedBlock);
  group.rawBlockName = textOrEmpty(insert->block_name);
  group.sampleX = insert->ins_pt.x;
  group.sampleY = insert->ins_pt.y;
  group.sampleZ = insert->ins_pt.z;
  group.scaleX = insert->scale.x;
  group.scaleY = insert->scale.y;
  group.scaleZ = insert->scale.z;
  group.rotation = insert->rotation;
  group.hasAttribs = insert->has_attribs != 0;
  group.numOwned = insert->num_owned;
  group.attributes = insertAttributes(dwg, insert);
}

void appendInsertGroupDiagnostics(const std::map<std::string, InsertGroupDebug>& groups, DrawingPreview& result) {
  if (groups.empty()) {
    return;
  }

  result.diagnostics.push_back({DiagnosticSeverity::Info, "Raw DWG insert groups:"});
  for (const auto& [_, group] : groups) {
    std::ostringstream stream;
    stream << "group='" << group.source << "'"
           << ", count=" << group.count
           << ", layer='" << group.layer << "'"
           << ", resolved_block='" << group.resolvedBlock << "'"
           << ", raw_block_name='" << group.rawBlockName << "'"
           << ", sample_xyz=(" << formatDouble(group.sampleX) << ", " << formatDouble(group.sampleY) << ", " << formatDouble(group.sampleZ) << ")"
           << ", scale=(" << formatDouble(group.scaleX) << ", " << formatDouble(group.scaleY) << ", " << formatDouble(group.scaleZ) << ")"
           << ", rotation=" << formatDouble(group.rotation)
           << ", has_attribs=" << (group.hasAttribs ? "true" : "false")
           << ", num_owned=" << group.numOwned;

    if (!group.attributes.empty()) {
      stream << ", attributes={" << group.attributes << "}";
    }

    result.diagnostics.push_back({DiagnosticSeverity::Info, stream.str()});
  }
}

void appendErrorName(std::ostringstream& stream, bool& first, const char* name) {
  if (!first) {
    stream << ", ";
  }
  stream << name;
  first = false;
}

std::string describeDwgError(int error) {
  std::ostringstream stream;
  stream << "GNU LibreDWG could not read the DWG file. Error code: " << error;

  bool first = true;
  std::ostringstream names;
  if ((error & DWG_ERR_WRONGCRC) != 0) {
    appendErrorName(names, first, "WRONGCRC");
  }
  if ((error & DWG_ERR_NOTYETSUPPORTED) != 0) {
    appendErrorName(names, first, "NOTYETSUPPORTED");
  }
  if ((error & DWG_ERR_UNHANDLEDCLASS) != 0) {
    appendErrorName(names, first, "UNHANDLEDCLASS");
  }
  if ((error & DWG_ERR_INVALIDTYPE) != 0) {
    appendErrorName(names, first, "INVALIDTYPE");
  }
  if ((error & DWG_ERR_INVALIDHANDLE) != 0) {
    appendErrorName(names, first, "INVALIDHANDLE");
  }
  if ((error & DWG_ERR_INVALIDEED) != 0) {
    appendErrorName(names, first, "INVALIDEED");
  }
  if ((error & DWG_ERR_VALUEOUTOFBOUNDS) != 0) {
    appendErrorName(names, first, "VALUEOUTOFBOUNDS");
  }
  if ((error & DWG_ERR_CLASSESNOTFOUND) != 0) {
    appendErrorName(names, first, "CLASSESNOTFOUND");
  }
  if ((error & DWG_ERR_SECTIONNOTFOUND) != 0) {
    appendErrorName(names, first, "SECTIONNOTFOUND");
  }
  if ((error & DWG_ERR_PAGENOTFOUND) != 0) {
    appendErrorName(names, first, "PAGENOTFOUND");
  }
  if ((error & DWG_ERR_INTERNALERROR) != 0) {
    appendErrorName(names, first, "INTERNALERROR");
  }
  if ((error & DWG_ERR_INVALIDDWG) != 0) {
    appendErrorName(names, first, "INVALIDDWG");
  }
  if ((error & DWG_ERR_IOERROR) != 0) {
    appendErrorName(names, first, "IOERROR");
  }
  if ((error & DWG_ERR_OUTOFMEM) != 0) {
    appendErrorName(names, first, "OUTOFMEM");
  }

  const auto nameText = names.str();
  if (!nameText.empty()) {
    stream << " (" << nameText << ")";
  }

  return stream.str();
}

struct DwgDataHolder {
  Dwg_Data data{};
  bool loaded = false;

  DwgDataHolder() {
    std::memset(&data, 0, sizeof(data));
  }

  ~DwgDataHolder() {
    if (loaded) {
      dwg_free(&data);
    }
  }
};

void appendPointEntities(Dwg_Data& dwg, DrawingPreview& result) {
  for (Dwg_Object* object = dwg_get_first_object(&dwg, DWG_TYPE_POINT); object != nullptr;
       object = dwg_get_next_object(&dwg, DWG_TYPE_POINT, object->index + 1)) {
    if (!object->tio.entity || !object->tio.entity->tio.POINT) {
      result.diagnostics.push_back({DiagnosticSeverity::Warning, "Skipped DWG POINT entity with missing data."});
      continue;
    }

    const auto* point = object->tio.entity->tio.POINT;
    result.points.push_back(Point{point->x, point->y, point->z, pointSource(entityLayerName(object->tio.entity))});
  }
}

void appendInsertEntities(Dwg_Data& dwg,
                          const std::vector<std::string>& allowedNames,
                          DrawingPreview& result) {
  std::map<std::string, InsertGroupDebug> groups;
  for (Dwg_Object* object = dwg_get_first_object(&dwg, DWG_TYPE_INSERT); object != nullptr;
       object = dwg_get_next_object(&dwg, DWG_TYPE_INSERT, object->index + 1)) {
    if (!object->tio.entity || !object->tio.entity->tio.INSERT) {
      result.diagnostics.push_back({DiagnosticSeverity::Warning, "Skipped DWG INSERT entity with missing data."});
      continue;
    }

    const auto* insert = object->tio.entity->tio.INSERT;
    const std::string resolvedBlockName = insertBlockName(dwg, insert);
    const std::string blockIdentity = handleIdentity(insert->block_header);
    const std::string blockName = blockDisplayName(resolvedBlockName, blockIdentity);
    if (!isAllowedInsert(allowedNames, resolvedBlockName) && !isAllowedInsert(allowedNames, blockName)) {
      continue;
    }

    const auto layer = entityLayerName(object->tio.entity);
    const auto source = insertSource(blockName, layer);
    rememberInsertGroup(groups, dwg, insert, source, layer, blockName);
    result.points.push_back(Point{insert->ins_pt.x, insert->ins_pt.y, insert->ins_pt.z, source});
  }

  appendInsertGroupDiagnostics(groups, result);
}

void appendLineEntities(Dwg_Data& dwg, DrawingPreview& preview) {
  for (Dwg_Object* object = dwg_get_first_object(&dwg, DWG_TYPE_LINE); object != nullptr;
       object = dwg_get_next_object(&dwg, DWG_TYPE_LINE, object->index + 1)) {
    if (!object->tio.entity || !object->tio.entity->tio.LINE) {
      continue;
    }

    const auto* line = object->tio.entity->tio.LINE;
    preview.lines.push_back(PreviewLine{line->start.x, line->start.y, line->end.x, line->end.y, entityLayerName(object->tio.entity)});
  }
}

void appendLwPolylineEntities(Dwg_Data& dwg, DrawingPreview& preview) {
  for (Dwg_Object* object = dwg_get_first_object(&dwg, DWG_TYPE_LWPOLYLINE); object != nullptr;
       object = dwg_get_next_object(&dwg, DWG_TYPE_LWPOLYLINE, object->index + 1)) {
    if (!object->tio.entity || !object->tio.entity->tio.LWPOLYLINE) {
      continue;
    }

    const auto* lwpolyline = object->tio.entity->tio.LWPOLYLINE;
    PreviewPolyline polyline;
    polyline.layer = entityLayerName(object->tio.entity);
    polyline.closed = (lwpolyline->flag & 512) != 0;
    for (BITCODE_BL index = 0; index < lwpolyline->num_points; ++index) {
      polyline.vertices.push_back(Point2d{lwpolyline->points[index].x, lwpolyline->points[index].y});
    }
    if (polyline.vertices.size() >= 2) {
      preview.polylines.push_back(std::move(polyline));
    }
  }
}

void appendClassicPolylineEntities(Dwg_Data& dwg, DrawingPreview& preview) {
  const auto appendPolyline = [&](Dwg_Object_Type type) {
    for (Dwg_Object* object = dwg_get_first_object(&dwg, type); object != nullptr;
         object = dwg_get_next_object(&dwg, type, object->index + 1)) {
      if (!object->tio.entity) {
        continue;
      }

      BITCODE_BL numOwned = 0;
      BITCODE_H* vertices = nullptr;
      bool closed = false;
      if (type == DWG_TYPE_POLYLINE_2D && object->tio.entity->tio.POLYLINE_2D) {
        const auto* poly = object->tio.entity->tio.POLYLINE_2D;
        numOwned = poly->num_owned;
        vertices = poly->vertex;
        closed = (poly->flag & 1) != 0;
      } else if (type == DWG_TYPE_POLYLINE_3D && object->tio.entity->tio.POLYLINE_3D) {
        const auto* poly = object->tio.entity->tio.POLYLINE_3D;
        numOwned = poly->num_owned;
        vertices = poly->vertex;
        closed = (poly->flag & 1) != 0;
      }

      PreviewPolyline polyline;
      polyline.layer = entityLayerName(object->tio.entity);
      polyline.closed = closed;
      for (BITCODE_BL index = 0; vertices && index < numOwned; ++index) {
        const auto* vertexObject = dwg_ref_object(&dwg, vertices[index]);
        if (!vertexObject || !vertexObject->tio.entity) {
          continue;
        }
        if (vertexObject->tio.entity->tio.VERTEX_2D) {
          const auto* vertex = vertexObject->tio.entity->tio.VERTEX_2D;
          polyline.vertices.push_back(Point2d{vertex->point.x, vertex->point.y});
        } else if (vertexObject->tio.entity->tio.VERTEX_3D) {
          const auto* vertex = vertexObject->tio.entity->tio.VERTEX_3D;
          polyline.vertices.push_back(Point2d{vertex->point.x, vertex->point.y});
        }
      }

      if (polyline.vertices.size() >= 2) {
        preview.polylines.push_back(std::move(polyline));
      }
    }
  };

  appendPolyline(DWG_TYPE_POLYLINE_2D);
  appendPolyline(DWG_TYPE_POLYLINE_3D);
}

void append3dFaceObject(Dwg_Object* object,
                        DrawingPreview& preview,
                        const InsertTransform* transform = nullptr,
                        const std::string* layerOverride = nullptr,
                        const std::string* sourceOverride = nullptr,
                        const std::string* blockOverride = nullptr) {
  if (!object || !object->tio.entity || !object->tio.entity->tio._3DFACE) {
    return;
  }

  const auto* face = object->tio.entity->tio._3DFACE;
  PreviewVertex p1{face->corner1.x, face->corner1.y, face->corner1.z};
  PreviewVertex p2{face->corner2.x, face->corner2.y, face->corner2.z};
  PreviewVertex p3{face->corner3.x, face->corner3.y, face->corner3.z};
  PreviewVertex p4{face->corner4.x, face->corner4.y, face->corner4.z};
  if (transform) {
    p1 = transformVertex(p1, *transform);
    p2 = transformVertex(p2, *transform);
    p3 = transformVertex(p3, *transform);
    p4 = transformVertex(p4, *transform);
  }

  const auto layer = layerOverride && !layerOverride->empty() ? *layerOverride : entityLayerName(object->tio.entity);
  appendTrianglePair(p1,
                     p2,
                     p3,
                     p4,
                     layer,
                     sourceOverride ? *sourceOverride : dgmSource("", layer),
                     blockOverride ? *blockOverride : "",
                     preview);
}

void append3dFaceEntities(Dwg_Data& dwg, DrawingPreview& preview) {
  for (Dwg_Object* object = dwg_get_first_object(&dwg, DWG_TYPE__3DFACE); object != nullptr;
       object = dwg_get_next_object(&dwg, DWG_TYPE__3DFACE, object->index + 1)) {
    if (!object->tio.entity || object->tio.entity->entmode != 2) {
      continue;
    }
    append3dFaceObject(object, preview);
  }
}

void appendInsertBlock3dFaces(Dwg_Data& dwg,
                              const std::vector<std::string>& allowedNames,
                              DrawingPreview& preview) {
  for (Dwg_Object* object = dwg_get_first_object(&dwg, DWG_TYPE_INSERT); object != nullptr;
       object = dwg_get_next_object(&dwg, DWG_TYPE_INSERT, object->index + 1)) {
    if (!object->tio.entity || !object->tio.entity->tio.INSERT) {
      continue;
    }

    const auto* insert = object->tio.entity->tio.INSERT;
    const std::string resolvedBlockName = insertBlockName(dwg, insert);
    const std::string blockIdentity = handleIdentity(insert->block_header);
    const std::string blockName = blockDisplayName(resolvedBlockName, blockIdentity);
    if (!isAllowedInsert(allowedNames, resolvedBlockName) && !isAllowedInsert(allowedNames, blockName)) {
      continue;
    }

    const auto* blockObject = dwg_ref_object(&dwg, insert->block_header);
    if (!blockObject || !blockObject->tio.object || !blockObject->tio.object->tio.BLOCK_HEADER) {
      continue;
    }

    const InsertTransform transform{insert->ins_pt.x, insert->ins_pt.y, insert->ins_pt.z, insert->scale.x, insert->scale.y, insert->scale.z, insert->rotation};
    const std::string layer = entityLayerName(object->tio.entity);
    const std::string source = dgmSource(blockName, layer, blockIdentity);
    const auto* blockHeader = blockObject->tio.object->tio.BLOCK_HEADER;
    for (BITCODE_BL index = 0; blockHeader->entities && index < blockHeader->num_owned; ++index) {
      Dwg_Object* entityObject = dwg_ref_object(&dwg, blockHeader->entities[index]);
      if (entityObject && entityObject->fixedtype == DWG_TYPE__3DFACE) {
        append3dFaceObject(entityObject, preview, &transform, &layer, &source, &blockName);
      }
    }
  }
}

void appendDgmTriangleDiagnostics(DrawingPreview& preview) {
  if (preview.triangles.empty()) {
    return;
  }

  std::map<std::string, std::size_t> groups;
  for (const auto& triangle : preview.triangles) {
    groups[triangle.source.empty() ? std::string("DGM") : triangle.source]++;
  }

  preview.diagnostics.push_back({DiagnosticSeverity::Info, "DGM triangle groups:"});
  for (const auto& [source, count] : groups) {
    std::ostringstream stream;
    stream << "group='" << source << "', triangles=" << count;
    preview.diagnostics.push_back({DiagnosticSeverity::Info, stream.str()});
  }
}

bool isPreviewSupportedType(enum DWG_OBJECT_TYPE type) {
  return type == DWG_TYPE_POINT || type == DWG_TYPE_INSERT || type == DWG_TYPE_LINE || type == DWG_TYPE_LWPOLYLINE ||
         type == DWG_TYPE_POLYLINE_2D || type == DWG_TYPE_POLYLINE_3D || type == DWG_TYPE_VERTEX_2D ||
         type == DWG_TYPE_VERTEX_3D || type == DWG_TYPE__3DFACE;
}

void appendIgnoredEntityDiagnostics(Dwg_Data& dwg, DrawingPreview& result) {
  std::map<std::string, std::size_t> ignored;
  for (BITCODE_BL index = 0; index < dwg.num_objects; ++index) {
    const auto& object = dwg.object[index];
    if (object.supertype != DWG_SUPERTYPE_ENTITY || isPreviewSupportedType(object.fixedtype)) {
      continue;
    }

    const auto name = textOrEmpty(object.dxfname ? object.dxfname : object.name);
    ignored[name.empty() ? "(unnamed)" : name]++;
  }

  if (ignored.empty()) {
    return;
  }

  std::ostringstream stream;
  stream << "Ignored DWG preview entities:";
  for (const auto& [type, count] : ignored) {
    stream << ' ' << type << '=' << count;
  }
  result.diagnostics.push_back({DiagnosticSeverity::Info, stream.str()});
}

void promoteLargestBlockGroupToDgm(DrawingPreview& result) {
  std::map<std::string, std::size_t> counts;
  bool hasDgmGroup = false;
  for (const auto& point : result.points) {
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

  for (auto& point : result.points) {
    if (point.source == largest->first) {
      point.source = "Geländemodell points";
    }
  }

  result.diagnostics.push_back({DiagnosticSeverity::Info, "Classified largest block point group as Geländemodell points."});
}
#endif

} // namespace

DwgPointReader::DwgPointReader(std::vector<std::string> allowedInsertBlockNames)
    : allowedInsertBlockNames_(std::move(allowedInsertBlockNames)) {}

ReadResult DwgPointReader::readPoints(const std::filesystem::path& file) const {
  return readPreview(file).toReadResult();
}

DrawingPreview DwgPointReader::readPreview(const std::filesystem::path& file) const {
  DrawingPreview result;

#ifndef DGM2XYZ_WITH_LIBREDWG
  (void)file;
  result.diagnostics.push_back({
      DiagnosticSeverity::Error,
      "DWG support was not built. Install GNU LibreDWG and configure with -DDGM2XYZ_LIBREDWG=ON.",
  });
  return result;
#else
  DwgDataHolder dwg;
  const auto fileName = pathToUtf8(file);
  const int error = dwg_read_file(fileName.c_str(), &dwg.data);
  if (error >= DWG_ERR_CRITICAL) {
    result.diagnostics.push_back({DiagnosticSeverity::Error, describeDwgError(error)});
    return result;
  }

  dwg.loaded = true;
  if (error != 0) {
    result.diagnostics.push_back({DiagnosticSeverity::Warning, describeDwgError(error)});
  }
  result.diagnostics.push_back({DiagnosticSeverity::Info, "GNU LibreDWG read the DWG file successfully."});
  appendPointEntities(dwg.data, result);
  appendInsertEntities(dwg.data, allowedInsertBlockNames_, result);
  promoteLargestBlockGroupToDgm(result);
  appendLineEntities(dwg.data, result);
  appendLwPolylineEntities(dwg.data, result);
  appendClassicPolylineEntities(dwg.data, result);
  append3dFaceEntities(dwg.data, result);
  appendInsertBlock3dFaces(dwg.data, allowedInsertBlockNames_, result);
  appendDgmTriangleDiagnostics(result);
  result.rebuildBounds();
  result.diagnostics.push_back({DiagnosticSeverity::Info, "Extracted " + std::to_string(result.points.size()) + " candidate DWG point object(s)."});
  result.diagnostics.push_back({DiagnosticSeverity::Info, "Preview background: " + std::to_string(result.lines.size()) + " line(s), " + std::to_string(result.polylines.size()) + " polyline(s), " + std::to_string(result.triangles.size()) + " triangle(s)."});
  appendIgnoredEntityDiagnostics(dwg.data, result);
  return result;
#endif
}

} // namespace dgm2xyz
