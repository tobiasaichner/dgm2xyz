#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace dgm2xyz {

struct Point {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  std::string source;
};

enum class DiagnosticSeverity {
  Info,
  Warning,
  Error,
};

struct Diagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::Info;
  std::string message;
};

struct ReadResult {
  std::vector<Point> points;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool hasErrors() const;
};

class CadPointReader {
public:
  virtual ~CadPointReader() = default;
  virtual ReadResult readPoints(const std::filesystem::path& file) const = 0;
};

} // namespace dgm2xyz
