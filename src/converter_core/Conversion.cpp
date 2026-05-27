#include "dgm2xyz/Conversion.h"

#include "dgm2xyz/XyzWriter.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <sstream>

namespace dgm2xyz {
namespace {

std::string lowerExtension(const std::filesystem::path& path) {
  auto extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return extension;
}

void appendDiagnostic(std::vector<Diagnostic>& diagnostics, DiagnosticSeverity severity, std::string message) {
  diagnostics.push_back(Diagnostic{severity, std::move(message)});
}

} // namespace

bool ReadResult::hasErrors() const {
  return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
    return diagnostic.severity == DiagnosticSeverity::Error;
  });
}

std::filesystem::path defaultOutputPath(const std::filesystem::path& inputPath) {
  auto outputPath = inputPath;
  outputPath.replace_extension(".xyz");
  return outputPath;
}

ConversionResult convertFile(const std::filesystem::path& inputPath, const CadPointReader& reader) {
  ConversionResult result;
  result.inputPath = inputPath;
  result.outputPath = defaultOutputPath(inputPath);

  const auto extension = lowerExtension(inputPath);
  if (extension == ".dwg") {
    result.status = ConversionStatus::Unsupported;
    appendDiagnostic(result.diagnostics, DiagnosticSeverity::Error, "DWG input is not supported yet.");
    return result;
  }

  if (extension != ".dxf") {
    result.status = ConversionStatus::Unsupported;
    appendDiagnostic(result.diagnostics, DiagnosticSeverity::Error, "Only DXF files are supported.");
    return result;
  }

  auto readResult = reader.readPoints(inputPath);
  result.diagnostics = std::move(readResult.diagnostics);

  if (readResult.hasErrors()) {
    result.status = ConversionStatus::Failed;
    return result;
  }

  if (readResult.points.empty()) {
    result.status = ConversionStatus::Failed;
    appendDiagnostic(result.diagnostics, DiagnosticSeverity::Error, "No supported point objects were found.");
    return result;
  }

  try {
    writeXyzFile(result.outputPath, readResult.points);
  } catch (const std::exception& exception) {
    result.status = ConversionStatus::Failed;
    appendDiagnostic(result.diagnostics, DiagnosticSeverity::Error, exception.what());
    return result;
  }

  result.pointCount = readResult.points.size();
  result.status = ConversionStatus::Succeeded;
  return result;
}

std::string ConversionResult::summary() const {
  switch (status) {
  case ConversionStatus::Succeeded: {
    std::ostringstream stream;
    stream << "Wrote " << pointCount << " point";
    if (pointCount != 1) {
      stream << "s";
    }
    return stream.str();
  }
  case ConversionStatus::Unsupported:
    return diagnostics.empty() ? "Unsupported file" : diagnostics.front().message;
  case ConversionStatus::Failed:
    return diagnostics.empty() ? "Conversion failed" : diagnostics.front().message;
  }

  return "Conversion failed";
}

} // namespace dgm2xyz
