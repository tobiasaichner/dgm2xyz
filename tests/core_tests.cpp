#include "dgm2xyz/Conversion.h"
#include "dgm2xyz/DwgPointReader.h"
#include "dgm2xyz/DxfPointReader.h"
#include "dgm2xyz/XyzWriter.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <string>

namespace {

class FakeReader final : public dgm2xyz::CadPointReader {
public:
  dgm2xyz::ReadResult readPoints(const std::filesystem::path&) const override {
    dgm2xyz::ReadResult result;
    result.points.push_back(dgm2xyz::Point{7.0, 8.0, 9.0, ""});
    return result;
  }
};

std::filesystem::path tempDir() {
  auto path = std::filesystem::temp_directory_path() / "dgm2xyz_tests";
  std::filesystem::create_directories(path);
  return path;
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary);
  output << content;
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

std::string minimalDxfWithEntity(const std::string& entity) {
  return "0\nSECTION\n2\nENTITIES\n" + entity + "0\nENDSEC\n0\nEOF\n";
}

void testXyzFormatting() {
  std::ostringstream output;
  dgm2xyz::writeXyz(output, {dgm2xyz::Point{1.0, 2.5, 3.125, ""}});
  assert(output.str() == "1.000 2.500 3.125\n");
}

void testDefaultOutputPath() {
  const auto output = dgm2xyz::defaultOutputPath("C:/data/input.dxf");
  assert(output.filename() == "input.xyz");
}

void testEmptyPointResultDoesNotCreateOutput() {
  const auto inputPath = tempDir() / "empty.dxf";
  const auto outputPath = tempDir() / "empty.xyz";
  std::filesystem::remove(outputPath);
  writeFile(inputPath, minimalDxfWithEntity(""));

  const auto result = dgm2xyz::convertFile(inputPath, dgm2xyz::DxfPointReader{});
  assert(result.status == dgm2xyz::ConversionStatus::Failed);
  assert(!std::filesystem::exists(outputPath));
}

void testPointEntityParsing() {
  const auto inputPath = tempDir() / "point.dxf";
  writeFile(inputPath, minimalDxfWithEntity("0\nPOINT\n10\n1.25\n20\n2.5\n30\n3.75\n"));

  const auto result = dgm2xyz::convertFile(inputPath, dgm2xyz::DxfPointReader{});
  assert(result.status == dgm2xyz::ConversionStatus::Succeeded);
  assert(readFile(tempDir() / "point.xyz") == "1.250 2.500 3.750\n");
}

void testInsertEntityParsing() {
  const auto inputPath = tempDir() / "insert.dxf";
  writeFile(inputPath, minimalDxfWithEntity("0\nINSERT\n2\nPOINT_BLOCK\n10\n100\n20\n200\n30\n12.5\n"));

  const auto result = dgm2xyz::convertFile(inputPath, dgm2xyz::DxfPointReader{});
  assert(result.status == dgm2xyz::ConversionStatus::Succeeded);
  assert(readFile(tempDir() / "insert.xyz") == "100.000 200.000 12.500\n");
}

void testSelectedSourceExport() {
  const auto inputPath = tempDir() / "selected.dxf";
  const auto outputPath = tempDir() / "selected.xyz";
  std::filesystem::remove(outputPath);

  const std::vector<dgm2xyz::Point> points{
      dgm2xyz::Point{1.0, 2.0, 3.0, "POINT"},
      dgm2xyz::Point{4.0, 5.0, 6.0, "INSERT: HEIGHT"},
  };

  const auto result = dgm2xyz::exportPoints(inputPath, points, std::set<std::string>{"INSERT: HEIGHT"});
  assert(result.status == dgm2xyz::ConversionStatus::Succeeded);
  assert(readFile(outputPath) == "4.000 5.000 6.000\n");
}

void testSelectedSourceExportRequiresSelection() {
  const auto inputPath = tempDir() / "not_selected.dxf";
  const auto outputPath = tempDir() / "not_selected.xyz";
  std::filesystem::remove(outputPath);

  const std::vector<dgm2xyz::Point> points{
      dgm2xyz::Point{1.0, 2.0, 3.0, "POINT"},
  };

  const auto result = dgm2xyz::exportPoints(inputPath, points, std::set<std::string>{"INSERT: HEIGHT"});
  assert(result.status == dgm2xyz::ConversionStatus::Failed);
  assert(!std::filesystem::exists(outputPath));
}

void testDwgExtensionDelegatesToReader() {
  const auto inputPath = tempDir() / "drawing.dwg";
  writeFile(inputPath, "not a real dwg");

  const auto result = dgm2xyz::convertFile(inputPath, FakeReader{});
  assert(result.status == dgm2xyz::ConversionStatus::Succeeded);
  assert(readFile(tempDir() / "drawing.xyz") == "7.000 8.000 9.000\n");
}

void testDwgReaderReturnsDiagnosticWhenLibreDwgIsNotBuilt() {
  const auto inputPath = tempDir() / "missing_libredwg.dwg";
  writeFile(inputPath, "not a real dwg");

  const auto result = dgm2xyz::DwgPointReader{}.readPoints(inputPath);
  if (result.hasErrors()) {
    assert(!result.diagnostics.empty());
  }
}

} // namespace

int main() {
  testXyzFormatting();
  testDefaultOutputPath();
  testEmptyPointResultDoesNotCreateOutput();
  testPointEntityParsing();
  testInsertEntityParsing();
  testSelectedSourceExport();
  testSelectedSourceExportRequiresSelection();
  testDwgExtensionDelegatesToReader();
  testDwgReaderReturnsDiagnosticWhenLibreDwgIsNotBuilt();
  return 0;
}
