#pragma once

#include "dgm2xyz/CadPointReader.h"

#include <filesystem>
#include <ostream>
#include <vector>

namespace dgm2xyz {

void writeXyz(std::ostream& output, const std::vector<Point>& points);
void writeXyzFile(const std::filesystem::path& outputPath, const std::vector<Point>& points);

} // namespace dgm2xyz
