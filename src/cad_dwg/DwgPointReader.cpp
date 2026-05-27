#include "dgm2xyz/DwgPointReader.h"

#include <algorithm>
#include <map>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

#ifdef DGM2XYZ_WITH_LIBREDWG
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

std::string insertBlockName(Dwg_Data& dwg, const Dwg_Entity_INSERT* insert) {
  if (!insert) {
    return {};
  }

  if (insert->block_name && insert->block_name[0] != '\0') {
    return insert->block_name;
  }

  const auto* blockObject = dwg_ref_object(&dwg, insert->block_header);
  if (!blockObject || !blockObject->tio.object || !blockObject->tio.object->tio.BLOCK_HEADER) {
    return {};
  }

  return textOrEmpty(blockObject->tio.object->tio.BLOCK_HEADER->name);
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

void appendInsertGroupDiagnostics(const std::map<std::string, InsertGroupDebug>& groups, ReadResult& result) {
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

void appendPointEntities(Dwg_Data& dwg, ReadResult& result) {
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
                          ReadResult& result) {
  std::map<std::string, InsertGroupDebug> groups;
  for (Dwg_Object* object = dwg_get_first_object(&dwg, DWG_TYPE_INSERT); object != nullptr;
       object = dwg_get_next_object(&dwg, DWG_TYPE_INSERT, object->index + 1)) {
    if (!object->tio.entity || !object->tio.entity->tio.INSERT) {
      result.diagnostics.push_back({DiagnosticSeverity::Warning, "Skipped DWG INSERT entity with missing data."});
      continue;
    }

    const auto* insert = object->tio.entity->tio.INSERT;
    const std::string blockName = insertBlockName(dwg, insert);
    if (!isAllowedInsert(allowedNames, blockName)) {
      continue;
    }

    const auto layer = entityLayerName(object->tio.entity);
    const auto source = insertSource(blockName, layer);
    rememberInsertGroup(groups, dwg, insert, source, layer, blockName);
    result.points.push_back(Point{insert->ins_pt.x, insert->ins_pt.y, insert->ins_pt.z, source});
  }

  appendInsertGroupDiagnostics(groups, result);
}

void promoteLargestBlockGroupToDgm(ReadResult& result) {
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
  ReadResult result;

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
  result.diagnostics.push_back({DiagnosticSeverity::Info, "Extracted " + std::to_string(result.points.size()) + " candidate DWG point object(s)."});
  return result;
#endif
}

} // namespace dgm2xyz
