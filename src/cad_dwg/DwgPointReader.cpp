#include "dgm2xyz/DwgPointReader.h"

#include <algorithm>
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

std::string pathToUtf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return std::string(value.begin(), value.end());
}

#ifdef DGM2XYZ_WITH_LIBREDWG
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
    result.points.push_back(Point{point->x, point->y, point->z, "POINT"});
  }
}

void appendInsertEntities(Dwg_Data& dwg,
                          const std::vector<std::string>& allowedNames,
                          ReadResult& result) {
  for (Dwg_Object* object = dwg_get_first_object(&dwg, DWG_TYPE_INSERT); object != nullptr;
       object = dwg_get_next_object(&dwg, DWG_TYPE_INSERT, object->index + 1)) {
    if (!object->tio.entity || !object->tio.entity->tio.INSERT) {
      result.diagnostics.push_back({DiagnosticSeverity::Warning, "Skipped DWG INSERT entity with missing data."});
      continue;
    }

    const auto* insert = object->tio.entity->tio.INSERT;
    const std::string blockName = insert->block_name ? insert->block_name : "";
    if (!isAllowedInsert(allowedNames, blockName)) {
      continue;
    }

    result.points.push_back(Point{insert->ins_pt.x, insert->ins_pt.y, insert->ins_pt.z, "INSERT: " + blockName});
  }
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
  result.diagnostics.push_back({DiagnosticSeverity::Info, "Extracted " + std::to_string(result.points.size()) + " candidate DWG point object(s)."});
  return result;
#endif
}

} // namespace dgm2xyz
