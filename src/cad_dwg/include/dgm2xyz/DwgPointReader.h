#pragma once

#include "dgm2xyz/CadPointReader.h"
#include "dgm2xyz/DrawingPreview.h"

#include <string>
#include <vector>

namespace dgm2xyz {

class DwgPointReader final : public CadPointReader {
public:
  explicit DwgPointReader(std::vector<std::string> allowedInsertBlockNames = {});

  ReadResult readPoints(const std::filesystem::path& file) const override;
  DrawingPreview readPreview(const std::filesystem::path& file) const;

private:
  std::vector<std::string> allowedInsertBlockNames_;
};

} // namespace dgm2xyz
