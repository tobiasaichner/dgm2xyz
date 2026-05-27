#include "dgm2xyz/DxfPointReader.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>

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
  double z = 0.0;
  std::string name;
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
         value == "ENDSEC" || value == "EOF";
}

bool isAllowedInsert(const std::vector<std::string>& allowedNames, const std::string& name) {
  if (allowedNames.empty()) {
    return true;
  }

  return std::find(allowedNames.begin(), allowedNames.end(), name) != allowedNames.end();
}

void finishEntity(const EntityState& entity,
                  const std::vector<std::string>& allowedNames,
                  ReadResult& result) {
  if (entity.type != "POINT" && entity.type != "INSERT") {
    return;
  }

  if (entity.type == "INSERT" && !isAllowedInsert(allowedNames, entity.name)) {
    return;
  }

  if (!entity.x || !entity.y) {
    result.diagnostics.push_back({DiagnosticSeverity::Warning, "Skipped " + entity.type + " entity with missing X or Y coordinate."});
    return;
  }

  result.points.push_back(Point{*entity.x, *entity.y, entity.z, entity.name});
}

} // namespace

DxfPointReader::DxfPointReader(std::vector<std::string> allowedInsertBlockNames)
    : allowedInsertBlockNames_(std::move(allowedInsertBlockNames)) {}

ReadResult DxfPointReader::readPoints(const std::filesystem::path& file) const {
  ReadResult result;

  std::ifstream input(file, std::ios::binary);
  if (!input) {
    result.diagnostics.push_back({DiagnosticSeverity::Error, "Could not open input file."});
    return result;
  }

  const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (containsNulByte(content)) {
    result.diagnostics.push_back({DiagnosticSeverity::Error, "Binary DXF is not supported by the initial reader."});
    return result;
  }

  auto pairs = parsePairs(content, result.diagnostics);
  if (result.hasErrors()) {
    return result;
  }

  EntityState current;
  bool inEntity = false;
  bool inEntitiesSection = false;
  bool awaitingSectionName = false;

  for (const auto& pair : pairs) {
    if (pair.code == 0 && pair.value == "SECTION") {
      awaitingSectionName = true;
      inEntitiesSection = false;
      continue;
    }

    if (awaitingSectionName && pair.code == 2) {
      inEntitiesSection = pair.value == "ENTITIES";
      awaitingSectionName = false;
      continue;
    }

    if (!inEntitiesSection) {
      continue;
    }

    if (pair.code == 0 && isEntityType(pair.value)) {
      if (inEntity) {
        finishEntity(current, allowedInsertBlockNames_, result);
      }

      current = EntityState{};
      current.type = pair.value;
      inEntity = pair.value == "POINT" || pair.value == "INSERT";
      continue;
    }

    if (!inEntity) {
      continue;
    }

    if (pair.code == 2 && current.type == "INSERT") {
      current.name = pair.value;
      continue;
    }

    if (pair.code == 10 || pair.code == 20 || pair.code == 30) {
      const auto value = parseDouble(pair.value);
      if (!value) {
        result.diagnostics.push_back({DiagnosticSeverity::Warning, "Skipped invalid numeric coordinate value."});
        continue;
      }

      if (pair.code == 10) {
        current.x = *value;
      } else if (pair.code == 20) {
        current.y = *value;
      } else {
        current.z = *value;
      }
    }
  }

  if (inEntity) {
    finishEntity(current, allowedInsertBlockNames_, result);
  }

  return result;
}

} // namespace dgm2xyz
