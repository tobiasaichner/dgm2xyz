#include "dgm2xyz/XyzWriter.h"

#include <fstream>
#include <iomanip>
#include <locale>
#include <stdexcept>

namespace dgm2xyz {

void writeXyz(std::ostream& output, const std::vector<Point>& points) {
  output.imbue(std::locale::classic());
  output << std::fixed << std::setprecision(3);

  for (const auto& point : points) {
    output << point.x << ' ' << point.y << ' ' << point.z << '\n';
  }
}

void writeXyzFile(const std::filesystem::path& outputPath, const std::vector<Point>& points) {
  std::ofstream output(outputPath, std::ios::binary);
  if (!output) {
    throw std::runtime_error("Could not open output file.");
  }

  writeXyz(output, points);
}

} // namespace dgm2xyz
