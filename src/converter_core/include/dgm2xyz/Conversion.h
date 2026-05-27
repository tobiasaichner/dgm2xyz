#pragma once

#include "dgm2xyz/CadPointReader.h"

#include <filesystem>
#include <string>
#include <vector>

namespace dgm2xyz {

enum class ConversionStatus {
  Succeeded,
  Failed,
  Unsupported,
};

struct ConversionResult {
  ConversionStatus status = ConversionStatus::Failed;
  std::filesystem::path inputPath;
  std::filesystem::path outputPath;
  std::size_t pointCount = 0;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] std::string summary() const;
};

[[nodiscard]] std::filesystem::path defaultOutputPath(const std::filesystem::path& inputPath);

ConversionResult convertFile(const std::filesystem::path& inputPath, const CadPointReader& reader);

} // namespace dgm2xyz
