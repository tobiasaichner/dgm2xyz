#include "dgm2xyz/Conversion.h"
#include "dgm2xyz/DrawingPreview.h"
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

void testPreviewBounds() {
  dgm2xyz::DrawingPreview preview;
  preview.points.push_back(dgm2xyz::Point{10.0, 20.0, 1.0, "POINT"});
  preview.lines.push_back(dgm2xyz::PreviewLine{-5.0, 3.0, 6.0, 40.0, "A"});
  preview.polylines.push_back(dgm2xyz::PreviewPolyline{{dgm2xyz::Point2d{100.0, -7.0}}, false, "B"});
  preview.triangles.push_back(dgm2xyz::PreviewTriangle{{120.0, 8.0, 1.0}, {121.0, 9.0, 2.0}, {122.0, 10.0, 3.0}, "C", "DGM layer: C", ""});
  preview.rebuildBounds();

  assert(preview.bounds.valid());
  assert(preview.bounds.minX == -5.0);
  assert(preview.bounds.minY == -7.0);
  assert(preview.bounds.maxX == 122.0);
  assert(preview.bounds.maxY == 40.0);
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

void testLinePreviewParsing() {
  const auto inputPath = tempDir() / "line_preview.dxf";
  writeFile(inputPath, minimalDxfWithEntity("0\nLINE\n8\nContours\n10\n1\n20\n2\n11\n3\n21\n4\n"));

  const auto preview = dgm2xyz::DxfPointReader{}.readPreview(inputPath);
  assert(!preview.hasErrors());
  assert(preview.lines.size() == 1);
  assert(preview.lines.front().x1 == 1.0);
  assert(preview.lines.front().y1 == 2.0);
  assert(preview.lines.front().x2 == 3.0);
  assert(preview.lines.front().y2 == 4.0);
  assert(preview.lines.front().layer == "Contours");
}

void testLwPolylinePreviewParsing() {
  const auto inputPath = tempDir() / "lwpolyline_preview.dxf";
  writeFile(inputPath, minimalDxfWithEntity("0\nLWPOLYLINE\n8\nBreaklines\n70\n1\n10\n1\n20\n2\n10\n3\n20\n4\n10\n5\n20\n6\n"));

  const auto preview = dgm2xyz::DxfPointReader{}.readPreview(inputPath);
  assert(!preview.hasErrors());
  assert(preview.polylines.size() == 1);
  assert(preview.polylines.front().closed);
  assert(preview.polylines.front().vertices.size() == 3);
  assert(preview.polylines.front().layer == "Breaklines");
}

void testClassicPolylinePreviewParsing() {
  const auto inputPath = tempDir() / "polyline_preview.dxf";
  writeFile(inputPath,
            minimalDxfWithEntity("0\nPOLYLINE\n8\nEdges\n70\n0\n"
                                 "0\nVERTEX\n8\nEdges\n10\n1\n20\n2\n"
                                 "0\nVERTEX\n8\nEdges\n10\n3\n20\n4\n"
                                 "0\nSEQEND\n"));

  const auto preview = dgm2xyz::DxfPointReader{}.readPreview(inputPath);
  assert(!preview.hasErrors());
  assert(preview.polylines.size() == 1);
  assert(!preview.polylines.front().closed);
  assert(preview.polylines.front().vertices.size() == 2);
  assert(preview.polylines.front().layer == "Edges");
}

void testPreviewCombinesPointBlocksAndBackground() {
  const auto inputPath = tempDir() / "combined_preview.dxf";
  writeFile(inputPath,
            minimalDxfWithEntity("0\nLINE\n8\nBackground\n10\n1\n20\n2\n11\n3\n21\n4\n"
                                 "0\nINSERT\n8\nGeländemodell_ALS-Punkt\n2\nFIGX20\n10\n100\n20\n200\n30\n12.5\n"));

  const auto preview = dgm2xyz::DxfPointReader{}.readPreview(inputPath);
  assert(!preview.hasErrors());
  assert(preview.lines.size() == 1);
  assert(preview.points.size() == 1);
  assert(preview.points.front().source == "Geländemodell points");
}

void test3dFacePreviewParsing() {
  const auto inputPath = tempDir() / "3dface_preview.dxf";
  writeFile(inputPath,
            minimalDxfWithEntity("0\n3DFACE\n8\nTIN\n10\n1\n20\n2\n30\n3\n11\n4\n21\n5\n31\n6\n12\n7\n22\n8\n32\n9\n13\n7\n23\n8\n33\n9\n"));

  const auto preview = dgm2xyz::DxfPointReader{}.readPreview(inputPath);
  assert(!preview.hasErrors());
  assert(preview.triangles.size() == 1);
  assert(preview.triangles.front().a.x == 1.0);
  assert(preview.triangles.front().b.y == 5.0);
  assert(preview.triangles.front().c.z == 9.0);
  assert(preview.triangles.front().layer == "TIN");
  assert(preview.triangles.front().source == "DGM | layer=TIN");
}

void testBlock3dFacePreviewParsing() {
  const auto inputPath = tempDir() / "block_3dface_preview.dxf";
  writeFile(inputPath,
            "0\nSECTION\n2\nBLOCKS\n"
            "0\nBLOCK\n2\nTRI_BLOCK\n"
            "0\n3DFACE\n8\nTIN_LOCAL\n10\n0\n20\n0\n30\n0\n11\n10\n21\n0\n31\n0\n12\n0\n22\n10\n32\n0\n13\n0\n23\n10\n33\n0\n"
            "0\nENDBLK\n0\nENDSEC\n"
            "0\nSECTION\n2\nENTITIES\n"
            "0\nINSERT\n8\nTIN_INSERT\n2\nTRI_BLOCK\n10\n100\n20\n200\n30\n0\n"
            "0\nENDSEC\n0\nEOF\n");

  const auto preview = dgm2xyz::DxfPointReader{}.readPreview(inputPath);
  assert(!preview.hasErrors());
  assert(preview.triangles.size() == 1);
  assert(preview.triangles.front().a.x == 100.0);
  assert(preview.triangles.front().a.y == 200.0);
  assert(preview.triangles.front().b.x == 110.0);
  assert(preview.triangles.front().c.z == 0.0);
  assert(preview.triangles.front().layer == "TIN_INSERT");
  assert(preview.triangles.front().source == "DGM | block=TRI_BLOCK | layer=TIN_INSERT");
  assert(preview.triangles.front().block == "TRI_BLOCK");
}

void testAnonymousDgmBlockNameIsPreserved() {
  const auto inputPath = tempDir() / "anonymous_block_3dface_preview.dxf";
  writeFile(inputPath,
            "0\nSECTION\n2\nBLOCKS\n"
            "0\nBLOCK\n2\n*U14\n"
            "0\n3DFACE\n8\nTIN_LOCAL\n10\n0\n20\n0\n30\n1\n11\n10\n21\n0\n31\n2\n12\n0\n22\n10\n32\n3\n13\n0\n23\n10\n33\n3\n"
            "0\nENDBLK\n0\nENDSEC\n"
            "0\nSECTION\n2\nENTITIES\n"
            "0\nINSERT\n8\nDGM_LAYER\n2\n*U14\n10\n100\n20\n200\n30\n10\n"
            "0\nENDSEC\n0\nEOF\n");

  const auto preview = dgm2xyz::DxfPointReader{}.readPreview(inputPath);
  assert(!preview.hasErrors());
  assert(preview.triangles.size() == 1);
  assert(preview.triangles.front().source == "DGM | block=*U14 | layer=DGM_LAYER");
  assert(preview.triangles.front().block == "*U14");
  assert(preview.triangles.front().a.z == 11.0);
}

void testDgmInsertSourceParsing() {
  const auto inputPath = tempDir() / "dgm_insert.dxf";
  writeFile(inputPath, minimalDxfWithEntity("0\nINSERT\n8\nGeländemodell_ALS-Punkt\n2\nFIGX20\n10\n100\n20\n200\n30\n12.5\n"));

  const auto result = dgm2xyz::DxfPointReader{}.readPoints(inputPath);
  assert(!result.hasErrors());
  assert(result.points.size() == 1);
  assert(result.points.front().source == "Geländemodell points");
}

void testDgmInsertSourceParsingIgnoresLayer() {
  const auto inputPath = tempDir() / "dgm_insert_other_layer.dxf";
  writeFile(inputPath, minimalDxfWithEntity("0\nINSERT\n8\nAnotherLayer\n2\nFIGX20\n10\n100\n20\n200\n30\n12.5\n"));

  const auto result = dgm2xyz::DxfPointReader{}.readPoints(inputPath);
  assert(!result.hasErrors());
  assert(result.points.size() == 1);
  assert(result.points.front().source == "Geländemodell points");
}

void testLargestGenericBlockGroupBecomesGelaendemodell() {
  const auto inputPath = tempDir() / "largest_dgm.dxf";
  writeFile(inputPath,
            minimalDxfWithEntity("0\nINSERT\n8\nB\n2\nF\n10\n1\n20\n1\n30\n1\n"
                                 "0\nINSERT\n8\nB\n2\nF\n10\n2\n20\n2\n30\n2\n"
                                 "0\nINSERT\n8\nK\n2\nF\n10\n3\n20\n3\n30\n3\n"));

  const auto result = dgm2xyz::DxfPointReader{}.readPoints(inputPath);
  assert(!result.hasErrors());
  assert(result.points.size() == 3);
  assert(result.points[0].source == "Geländemodell points");
  assert(result.points[1].source == "Geländemodell points");
  assert(result.points[2].source == "Layer: K");
}

void testGenericInsertSourceIsDescriptive() {
  const auto inputPath = tempDir() / "generic_insert.dxf";
  writeFile(inputPath, minimalDxfWithEntity("0\nINSERT\n8\nB\n2\nG\n10\n100\n20\n200\n30\n12.5\n"));

  const auto result = dgm2xyz::DxfPointReader{}.readPoints(inputPath);
  assert(!result.hasErrors());
  assert(result.points.size() == 1);
  assert(result.points.front().source == "Geländemodell points");
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
  testPreviewBounds();
  testXyzFormatting();
  testDefaultOutputPath();
  testEmptyPointResultDoesNotCreateOutput();
  testPointEntityParsing();
  testInsertEntityParsing();
  testLinePreviewParsing();
  testLwPolylinePreviewParsing();
  testClassicPolylinePreviewParsing();
  testPreviewCombinesPointBlocksAndBackground();
  test3dFacePreviewParsing();
  testBlock3dFacePreviewParsing();
  testAnonymousDgmBlockNameIsPreserved();
  testDgmInsertSourceParsing();
  testDgmInsertSourceParsingIgnoresLayer();
  testLargestGenericBlockGroupBecomesGelaendemodell();
  testGenericInsertSourceIsDescriptive();
  testSelectedSourceExport();
  testSelectedSourceExportRequiresSelection();
  testDwgExtensionDelegatesToReader();
  testDwgReaderReturnsDiagnosticWhenLibreDwgIsNotBuilt();
  return 0;
}
