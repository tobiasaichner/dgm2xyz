#include "dgm2xyz/Conversion.h"
#include "dgm2xyz/DrawingPreview.h"
#include "dgm2xyz/DwgPointReader.h"
#include "dgm2xyz/DxfPointReader.h"
#include "dgm2xyz/XyzWriter.h"

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <d2d1.h>
#include <shlobj.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr int kPickButtonId = 1001;
constexpr int kExportButtonId = 1002;
constexpr int kDropLabelId = 1003;
constexpr int kTypeListId = 1005;
constexpr int kStatusLabelId = 1006;
constexpr int kPreviewViewId = 1007;
constexpr int kTextPreviewId = 1008;
constexpr int kOutputEditId = 1009;
constexpr UINT kPreviewDoneMessage = WM_APP + 1;
constexpr UINT kExportDoneMessage = WM_APP + 2;

std::mutex gLogMutex;
std::filesystem::path gLogPath;
const wchar_t* kPreviewClassName = L"dgm2xyz.PreviewView";
const wchar_t* kDropAreaClassName = L"dgm2xyz.DropArea";

struct AppSettings {
  int windowX = CW_USEDEFAULT;
  int windowY = CW_USEDEFAULT;
  int windowWidth = 980;
  int windowHeight = 720;
  bool maximized = false;
  double listSplitRatio = 0.42;
  double previewHeightRatio = 0.36;
  std::filesystem::path lastOpenDirectory;
};

AppSettings gSettings;
std::filesystem::path gSettingsPath;

enum class SplitterDrag {
  None,
  VerticalLists,
  HorizontalPreview,
};

struct FilePreview {
  std::filesystem::path path;
  dgm2xyz::DrawingPreview drawingPreview;
  dgm2xyz::ReadResult readResult;
  bool loaded = false;
};

struct AppState {
  HWND pickButton = nullptr;
  HWND exportButton = nullptr;
  HWND outputEdit = nullptr;
  HWND dropLabel = nullptr;
  HWND typeList = nullptr;
  HWND textPreview = nullptr;
  HWND previewView = nullptr;
  HWND statusLabel = nullptr;
  HFONT uiFont = nullptr;
  FilePreview file;
  bool hasFile = false;
  double listSplitRatio = 0.42;
  double previewHeightRatio = 0.36;
  UINT currentDpi = 96;
  RECT verticalSplitter{};
  RECT horizontalSplitter{};
  SplitterDrag splitterDrag = SplitterDrag::None;
};

struct FinishedPreview {
  std::filesystem::path path;
  dgm2xyz::DrawingPreview preview;
};

struct FinishedExport {
  dgm2xyz::ConversionResult result;
};

struct SourceDisplay {
  std::wstring type;
  std::wstring block;
  std::wstring layer;
};

struct DropAreaState {
  HFONT font = nullptr;
};

void layoutControls(HWND window, AppState& state, bool redrawChildren);

class AppPointReader final : public dgm2xyz::CadPointReader {
public:
  dgm2xyz::ReadResult readPoints(const std::filesystem::path& file) const override {
    return readPreview(file).toReadResult();
  }

  dgm2xyz::DrawingPreview readPreview(const std::filesystem::path& file) const {
    const auto extension = lowerExtension(file.extension().wstring());
    if (extension == L".dwg") {
      return dwgReader_.readPreview(file);
    }

    return dxfReader_.readPreview(file);
  }

private:
  static std::wstring lowerExtension(std::wstring extension) {
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) {
      return static_cast<wchar_t>(towlower(c));
    });
    return extension;
  }

  dgm2xyz::DxfPointReader dxfReader_;
  dgm2xyz::DwgPointReader dwgReader_;
};

template <typename T>
void safeRelease(T*& value) {
  if (value) {
    value->Release();
    value = nullptr;
  }
}

struct PreviewViewState {
  dgm2xyz::DrawingPreview preview;
  std::set<std::string> selectedSources;
  ID2D1Factory* factory = nullptr;
  ID2D1HwndRenderTarget* renderTarget = nullptr;
  ID2D1SolidColorBrush* lineBrush = nullptr;
  ID2D1SolidColorBrush* triangleFillBrush = nullptr;
  ID2D1SolidColorBrush* selectedTriangleFillBrush = nullptr;
  ID2D1SolidColorBrush* triangleOutlineBrush = nullptr;
  ID2D1SolidColorBrush* mutedPointBrush = nullptr;
  ID2D1SolidColorBrush* selectedPointBrush = nullptr;
  ID2D1SolidColorBrush* dgmPointBrush = nullptr;
  ID2D1SolidColorBrush* borderBrush = nullptr;
  double centerX = 0.0;
  double centerY = 0.0;
  double scale = 1.0;
  bool needsFit = true;
  bool userViewChanged = false;
  bool panning = false;
  POINT lastMouse{};

  ~PreviewViewState() {
    safeRelease(borderBrush);
    safeRelease(dgmPointBrush);
    safeRelease(selectedPointBrush);
    safeRelease(mutedPointBrush);
    safeRelease(triangleOutlineBrush);
    safeRelease(selectedTriangleFillBrush);
    safeRelease(triangleFillBrush);
    safeRelease(lineBrush);
    safeRelease(renderTarget);
    safeRelease(factory);
  }
};

PreviewViewState* previewViewState(HWND previewView) {
  return reinterpret_cast<PreviewViewState*>(GetWindowLongPtrW(previewView, GWLP_USERDATA));
}

RECT clientRect(HWND window) {
  RECT rect{};
  GetClientRect(window, &rect);
  return rect;
}

int scaleForDpi(int value, UINT dpi) {
  return MulDiv(value, static_cast<int>(dpi), 96);
}

float scaleForDpi(float value, UINT dpi) {
  return value * static_cast<float>(dpi) / 96.0F;
}

UINT windowDpi(HWND window) {
  const UINT dpi = GetDpiForWindow(window);
  return dpi == 0 ? 96 : dpi;
}

HFONT createUiFont(UINT dpi) {
  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
    return CreateFontIndirectW(&metrics.lfMessageFont);
  }

  return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

void releaseUiFont(HFONT font) {
  if (font && font != GetStockObject(DEFAULT_GUI_FONT)) {
    DeleteObject(font);
  }
}

void applyUiFont(const AppState& state) {
  for (HWND handle : {state.pickButton,
                      state.exportButton,
                      state.outputEdit,
                      state.dropLabel,
                      state.typeList,
                      state.textPreview,
                      state.previewView,
                      state.statusLabel}) {
    if (handle) {
      SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(state.uiFont), TRUE);
    }
  }
}

void recreateUiFont(AppState& state, UINT dpi) {
  releaseUiFont(state.uiFont);
  state.currentDpi = dpi == 0 ? 96 : dpi;
  state.uiFont = createUiFont(state.currentDpi);
  applyUiFont(state);
}

bool ensurePreviewResources(HWND previewView, PreviewViewState& state) {
  if (!state.factory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &state.factory))) {
    return false;
  }

  if (!state.renderTarget) {
    const RECT rect = clientRect(previewView);
    const auto size = D2D1::SizeU(static_cast<UINT32>(std::max(1L, rect.right - rect.left)),
                                  static_cast<UINT32>(std::max(1L, rect.bottom - rect.top)));
    if (FAILED(state.factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                                     D2D1::HwndRenderTargetProperties(previewView, size),
                                                     &state.renderTarget))) {
      return false;
    }

    state.renderTarget->CreateSolidColorBrush(D2D1::ColorF(0xD8DDE3), &state.lineBrush);
    state.renderTarget->CreateSolidColorBrush(D2D1::ColorF(0xE8EEF5, 0.55F), &state.triangleFillBrush);
    state.renderTarget->CreateSolidColorBrush(D2D1::ColorF(0xF2A66A, 0.46F), &state.selectedTriangleFillBrush);
    state.renderTarget->CreateSolidColorBrush(D2D1::ColorF(0xB6C4D4), &state.triangleOutlineBrush);
    state.renderTarget->CreateSolidColorBrush(D2D1::ColorF(0xA9B2BD), &state.mutedPointBrush);
    state.renderTarget->CreateSolidColorBrush(D2D1::ColorF(0x1D6FD1), &state.selectedPointBrush);
    state.renderTarget->CreateSolidColorBrush(D2D1::ColorF(0xE05A27), &state.dgmPointBrush);
    state.renderTarget->CreateSolidColorBrush(D2D1::ColorF(0xB7C0CA), &state.borderBrush);
  }

  return true;
}

D2D1_POINT_2F worldToScreen(const PreviewViewState& state, double x, double y) {
  const auto size = state.renderTarget->GetSize();
  const double screenX = static_cast<double>(size.width) / 2.0 + (x - state.centerX) * state.scale;
  const double screenY = static_cast<double>(size.height) / 2.0 - (y - state.centerY) * state.scale;
  return D2D1::Point2F(static_cast<float>(screenX), static_cast<float>(screenY));
}

dgm2xyz::Point2d screenToWorld(const PreviewViewState& state, float x, float y) {
  if (state.scale <= 0.0 || !state.renderTarget) {
    return {};
  }
  const auto size = state.renderTarget->GetSize();
  return dgm2xyz::Point2d{state.centerX + (static_cast<double>(x) - static_cast<double>(size.width) / 2.0) / state.scale,
                          state.centerY - (static_cast<double>(y) - static_cast<double>(size.height) / 2.0) / state.scale};
}

void appendPreviewCoordinate(const dgm2xyz::Point2d& point, std::vector<double>& xs, std::vector<double>& ys) {
  xs.push_back(point.x);
  ys.push_back(point.y);
}

double percentile(std::vector<double> values, double fraction) {
  if (values.empty()) {
    return 0.0;
  }

  std::sort(values.begin(), values.end());
  const auto index = static_cast<std::size_t>(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(values.size() - 1));
  return values[index];
}

dgm2xyz::Bounds2d robustFitBounds(const dgm2xyz::DrawingPreview& preview) {
  std::vector<double> xs;
  std::vector<double> ys;
  xs.reserve(preview.points.size() + preview.lines.size() * 2);
  ys.reserve(preview.points.size() + preview.lines.size() * 2);

  for (const auto& point : preview.points) {
    appendPreviewCoordinate(dgm2xyz::Point2d{point.x, point.y}, xs, ys);
  }

  for (const auto& line : preview.lines) {
    appendPreviewCoordinate(dgm2xyz::Point2d{line.x1, line.y1}, xs, ys);
    appendPreviewCoordinate(dgm2xyz::Point2d{line.x2, line.y2}, xs, ys);
  }

  for (const auto& polyline : preview.polylines) {
    for (const auto& vertex : polyline.vertices) {
      appendPreviewCoordinate(vertex, xs, ys);
    }
  }

  for (const auto& triangle : preview.triangles) {
    appendPreviewCoordinate(dgm2xyz::Point2d{triangle.a.x, triangle.a.y}, xs, ys);
    appendPreviewCoordinate(dgm2xyz::Point2d{triangle.b.x, triangle.b.y}, xs, ys);
    appendPreviewCoordinate(dgm2xyz::Point2d{triangle.c.x, triangle.c.y}, xs, ys);
  }

  if (xs.size() < 12 || ys.size() < 12) {
    return preview.bounds;
  }

  const double q1x = percentile(xs, 0.25);
  const double q3x = percentile(xs, 0.75);
  const double q1y = percentile(ys, 0.25);
  const double q3y = percentile(ys, 0.75);
  const double iqrX = std::max(1.0, q3x - q1x);
  const double iqrY = std::max(1.0, q3y - q1y);
  const double lowX = q1x - iqrX * 6.0;
  const double highX = q3x + iqrX * 6.0;
  const double lowY = q1y - iqrY * 6.0;
  const double highY = q3y + iqrY * 6.0;

  dgm2xyz::Bounds2d bounds;
  std::size_t included = 0;
  for (std::size_t index = 0; index < xs.size(); ++index) {
    if (xs[index] < lowX || xs[index] > highX || ys[index] < lowY || ys[index] > highY) {
      continue;
    }
    bounds.include(xs[index], ys[index]);
    ++included;
  }

  if (!bounds.valid() || included < std::max<std::size_t>(3, xs.size() / 4)) {
    return preview.bounds;
  }

  return bounds;
}

void fitPreviewToView(HWND previewView, PreviewViewState& state) {
  const RECT rect = clientRect(previewView);
  const float width = static_cast<float>(std::max(1L, rect.right - rect.left));
  const float height = static_cast<float>(std::max(1L, rect.bottom - rect.top));
  const float padding = scaleForDpi(18.0F, windowDpi(previewView));

  const auto fitBounds = robustFitBounds(state.preview);
  if (!fitBounds.valid()) {
    state.centerX = 0.0;
    state.centerY = 0.0;
    state.scale = 1.0;
    state.needsFit = false;
    return;
  }

  const auto boundsWidth = static_cast<float>(std::max(1.0, fitBounds.width()));
  const auto boundsHeight = static_cast<float>(std::max(1.0, fitBounds.height()));
  const float availableWidth = std::max(1.0F, width - padding * 2.0F);
  const float availableHeight = std::max(1.0F, height - padding * 2.0F);
  state.centerX = (fitBounds.minX + fitBounds.maxX) / 2.0;
  state.centerY = (fitBounds.minY + fitBounds.maxY) / 2.0;
  state.scale = std::max(0.001, static_cast<double>(std::min(availableWidth / boundsWidth, availableHeight / boundsHeight)));
  state.needsFit = false;
}

bool worldSegmentIntersects(const dgm2xyz::Bounds2d& viewport, double x1, double y1, double x2, double y2) {
  const double minX = std::min(x1, x2);
  const double maxX = std::max(x1, x2);
  const double minY = std::min(y1, y2);
  const double maxY = std::max(y1, y2);
  return maxX >= viewport.minX && minX <= viewport.maxX && maxY >= viewport.minY && minY <= viewport.maxY;
}

bool worldTriangleIntersects(const dgm2xyz::Bounds2d& viewport, const dgm2xyz::PreviewTriangle& triangle) {
  dgm2xyz::Bounds2d bounds;
  bounds.include(triangle.a.x, triangle.a.y);
  bounds.include(triangle.b.x, triangle.b.y);
  bounds.include(triangle.c.x, triangle.c.y);
  return bounds.maxX >= viewport.minX && bounds.minX <= viewport.maxX && bounds.maxY >= viewport.minY &&
         bounds.minY <= viewport.maxY;
}

dgm2xyz::Bounds2d currentWorldViewport(const PreviewViewState& state) {
  dgm2xyz::Bounds2d viewport;
  if (!state.renderTarget || state.scale <= 0.0) {
    return viewport;
  }

  const auto size = state.renderTarget->GetSize();
  const double halfWidth = static_cast<double>(size.width) / (2.0 * state.scale);
  const double halfHeight = static_cast<double>(size.height) / (2.0 * state.scale);
  const double pad = 12.0 / state.scale;
  viewport.include(state.centerX - halfWidth - pad, state.centerY - halfHeight - pad);
  viewport.include(state.centerX + halfWidth + pad, state.centerY + halfHeight + pad);
  return viewport;
}

bool isSelectedSource(const PreviewViewState& state, const std::string& source) {
  return state.selectedSources.find(source) != state.selectedSources.end();
}

ID2D1SolidColorBrush* pointBrushForSource(PreviewViewState& state, const std::string& source, bool selected) {
  if (!selected) {
    return state.mutedPointBrush;
  }
  if (source == "Geländemodell points") {
    return state.dgmPointBrush;
  }
  return state.selectedPointBrush;
}

void drawPreview(HWND previewView, PreviewViewState& state) {
  if (!ensurePreviewResources(previewView, state)) {
    return;
  }

  if (state.needsFit) {
    fitPreviewToView(previewView, state);
  }

  const RECT rect = clientRect(previewView);
  const auto viewport = currentWorldViewport(state);
  const UINT dpi = windowDpi(previewView);
  const float lineWidth = scaleForDpi(1.0F, dpi);
  const float borderWidth = scaleForDpi(1.0F, dpi);
  const float vertexRadius = scaleForDpi(4.0F, dpi);
  const float selectedRadius = scaleForDpi(3.5F, dpi);
  const float mutedRadius = scaleForDpi(2.0F, dpi);
  state.renderTarget->BeginDraw();
  state.renderTarget->Clear(D2D1::ColorF(0xFFFFFF));

  const auto border = D2D1::RectF(0.5F,
                                  0.5F,
                                  static_cast<float>(std::max(1L, rect.right - rect.left)) - 0.5F,
                                  static_cast<float>(std::max(1L, rect.bottom - rect.top)) - 0.5F);
  state.renderTarget->DrawRectangle(border, state.borderBrush, borderWidth);

  for (const auto& triangle : state.preview.triangles) {
    if (!worldTriangleIntersects(viewport, triangle)) {
      continue;
    }

    ID2D1PathGeometry* geometry = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (state.factory && SUCCEEDED(state.factory->CreatePathGeometry(&geometry)) &&
        SUCCEEDED(geometry->Open(&sink))) {
      sink->BeginFigure(worldToScreen(state, triangle.a.x, triangle.a.y), D2D1_FIGURE_BEGIN_FILLED);
      sink->AddLine(worldToScreen(state, triangle.b.x, triangle.b.y));
      sink->AddLine(worldToScreen(state, triangle.c.x, triangle.c.y));
      sink->EndFigure(D2D1_FIGURE_END_CLOSED);
      sink->Close();
      state.renderTarget->FillGeometry(geometry,
                                       isSelectedSource(state, triangle.source) ? state.selectedTriangleFillBrush
                                                                               : state.triangleFillBrush);
      state.renderTarget->DrawGeometry(geometry, state.triangleOutlineBrush, lineWidth);
    }
    safeRelease(sink);
    safeRelease(geometry);
  }

  for (const auto& line : state.preview.lines) {
    if (!worldSegmentIntersects(viewport, line.x1, line.y1, line.x2, line.y2)) {
      continue;
    }
    const auto a = worldToScreen(state, line.x1, line.y1);
    const auto b = worldToScreen(state, line.x2, line.y2);
    state.renderTarget->DrawLine(a, b, state.lineBrush, lineWidth);
  }

  for (const auto& polyline : state.preview.polylines) {
    if (polyline.vertices.size() < 2) {
      continue;
    }
    for (std::size_t index = 1; index < polyline.vertices.size(); ++index) {
      if (!worldSegmentIntersects(viewport,
                                  polyline.vertices[index - 1].x,
                                  polyline.vertices[index - 1].y,
                                  polyline.vertices[index].x,
                                  polyline.vertices[index].y)) {
        continue;
      }
      const auto a = worldToScreen(state, polyline.vertices[index - 1].x, polyline.vertices[index - 1].y);
      const auto b = worldToScreen(state, polyline.vertices[index].x, polyline.vertices[index].y);
      state.renderTarget->DrawLine(a, b, state.lineBrush, lineWidth);
    }
    if (polyline.closed) {
      if (!worldSegmentIntersects(viewport,
                                  polyline.vertices.back().x,
                                  polyline.vertices.back().y,
                                  polyline.vertices.front().x,
                                  polyline.vertices.front().y)) {
        continue;
      }
      const auto a = worldToScreen(state, polyline.vertices.back().x, polyline.vertices.back().y);
      const auto b = worldToScreen(state, polyline.vertices.front().x, polyline.vertices.front().y);
      state.renderTarget->DrawLine(a, b, state.lineBrush, lineWidth);
    }
  }

  for (const auto& point : state.preview.points) {
    if (point.x < viewport.minX || point.x > viewport.maxX || point.y < viewport.minY || point.y > viewport.maxY) {
      continue;
    }
    const bool selected = isSelectedSource(state, point.source);
    const auto center = worldToScreen(state, point.x, point.y);
    if (center.x < rect.left - 8.0F || center.x > rect.right + 8.0F || center.y < rect.top - 8.0F ||
        center.y > rect.bottom + 8.0F) {
      continue;
    }
    const float radius = selected ? selectedRadius : mutedRadius;
    state.renderTarget->FillEllipse(D2D1::Ellipse(center, radius, radius),
                                    pointBrushForSource(state, point.source, selected));
  }

  const auto drawSelectedVertex = [&](const dgm2xyz::PreviewVertex& vertex) {
    if (vertex.x < viewport.minX || vertex.x > viewport.maxX || vertex.y < viewport.minY || vertex.y > viewport.maxY) {
      return;
    }
    const auto center = worldToScreen(state, vertex.x, vertex.y);
    if (center.x < rect.left - 8.0F || center.x > rect.right + 8.0F || center.y < rect.top - 8.0F ||
        center.y > rect.bottom + 8.0F) {
      return;
    }
    const auto ellipse = D2D1::Ellipse(center, vertexRadius, vertexRadius);
    state.renderTarget->FillEllipse(ellipse, state.selectedPointBrush);
    state.renderTarget->DrawEllipse(ellipse, state.borderBrush, borderWidth);
  };

  for (const auto& triangle : state.preview.triangles) {
    if (!isSelectedSource(state, triangle.source) || !worldTriangleIntersects(viewport, triangle)) {
      continue;
    }
    drawSelectedVertex(triangle.a);
    drawSelectedVertex(triangle.b);
    drawSelectedVertex(triangle.c);
  }

  if (state.renderTarget->EndDraw() == D2DERR_RECREATE_TARGET) {
    safeRelease(state.borderBrush);
    safeRelease(state.dgmPointBrush);
    safeRelease(state.selectedPointBrush);
    safeRelease(state.mutedPointBrush);
    safeRelease(state.triangleOutlineBrush);
    safeRelease(state.selectedTriangleFillBrush);
    safeRelease(state.triangleFillBrush);
    safeRelease(state.lineBrush);
    safeRelease(state.renderTarget);
  }
}

void setPreviewData(HWND previewView, dgm2xyz::DrawingPreview preview, std::set<std::string> selectedSources) {
  auto* state = previewViewState(previewView);
  if (!state) {
    return;
  }

  state->preview = std::move(preview);
  state->selectedSources = std::move(selectedSources);
  state->needsFit = true;
  state->userViewChanged = false;
  InvalidateRect(previewView, nullptr, FALSE);
}

void setPreviewSelection(HWND previewView, std::set<std::string> selectedSources) {
  auto* state = previewViewState(previewView);
  if (!state) {
    return;
  }

  state->selectedSources = std::move(selectedSources);
  InvalidateRect(previewView, nullptr, FALSE);
}

LRESULT CALLBACK previewWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  auto* state = previewViewState(window);

  switch (message) {
  case WM_NCCREATE:
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new PreviewViewState()));
    return TRUE;

  case WM_SIZE:
    if (state && state->renderTarget) {
      const RECT rect = clientRect(window);
      state->renderTarget->Resize(D2D1::SizeU(static_cast<UINT32>(std::max(1L, rect.right - rect.left)),
                                              static_cast<UINT32>(std::max(1L, rect.bottom - rect.top))));
      if (!state->userViewChanged) {
        state->needsFit = true;
      }
      InvalidateRect(window, nullptr, FALSE);
    }
    return 0;

  case WM_PAINT:
    if (state) {
      PAINTSTRUCT paint{};
      BeginPaint(window, &paint);
      drawPreview(window, *state);
      EndPaint(window, &paint);
      return 0;
    }
    break;

  case WM_MOUSEWHEEL:
    if (state) {
      ensurePreviewResources(window, *state);
      const double factor = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1.15 : 1.0 / 1.15;
      POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      ScreenToClient(window, &cursor);
      const auto world = screenToWorld(*state, static_cast<float>(cursor.x), static_cast<float>(cursor.y));
      state->scale = std::clamp(state->scale * factor, 0.000001, 1000000000.0);
      const auto size = state->renderTarget->GetSize();
      state->centerX = world.x - (static_cast<double>(cursor.x) - static_cast<double>(size.width) / 2.0) / state->scale;
      state->centerY = world.y + (static_cast<double>(cursor.y) - static_cast<double>(size.height) / 2.0) / state->scale;
      state->needsFit = false;
      state->userViewChanged = true;
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    break;

  case WM_LBUTTONDOWN:
    if (state) {
      state->panning = true;
      state->lastMouse = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      SetCapture(window);
      return 0;
    }
    break;

  case WM_MOUSEMOVE:
    if (state && state->panning) {
      const POINT current{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      if (state->scale > 0.0) {
        state->centerX -= static_cast<double>(current.x - state->lastMouse.x) / state->scale;
        state->centerY += static_cast<double>(current.y - state->lastMouse.y) / state->scale;
      }
      state->lastMouse = current;
      state->needsFit = false;
      state->userViewChanged = true;
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    break;

  case WM_LBUTTONUP:
    if (state && state->panning) {
      state->panning = false;
      ReleaseCapture();
      return 0;
    }
    break;

  case WM_LBUTTONDBLCLK:
    if (state) {
      state->needsFit = true;
      state->userViewChanged = false;
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    break;

  case WM_NCDESTROY:
    delete state;
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    return 0;
  }

  return DefWindowProcW(window, message, wParam, lParam);
}

DropAreaState* dropAreaState(HWND window) {
  return reinterpret_cast<DropAreaState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void drawDropArea(HWND window, DropAreaState& state) {
  PAINTSTRUCT paint{};
  HDC targetDc = BeginPaint(window, &paint);
  HDC dc = targetDc;
  HPAINTBUFFER buffer = BeginBufferedPaint(targetDc, &paint.rcPaint, BPBF_COMPATIBLEBITMAP, nullptr, &dc);
  if (!buffer || !dc) {
    dc = targetDc;
  }

  RECT rect = clientRect(window);
  FillRect(dc, &rect, GetSysColorBrush(COLOR_WINDOW));
  DrawEdge(dc, &rect, EDGE_ETCHED, BF_RECT);
  InflateRect(&rect, -scaleForDpi(10, windowDpi(window)), 0);

  std::wstring text;
  const int textLength = GetWindowTextLengthW(window);
  if (textLength > 0) {
    std::vector<wchar_t> buffer(static_cast<std::size_t>(textLength) + 1, L'\0');
    GetWindowTextW(window, buffer.data(), static_cast<int>(buffer.size()));
    text.assign(buffer.data());
  }

  const HFONT oldFont = state.font ? static_cast<HFONT>(SelectObject(dc, state.font)) : nullptr;
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
  DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  if (oldFont) {
    SelectObject(dc, oldFont);
  }

  if (buffer) {
    EndBufferedPaint(buffer, TRUE);
  }
  EndPaint(window, &paint);
}

LRESULT CALLBACK dropAreaWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  auto* state = dropAreaState(window);

  switch (message) {
  case WM_NCCREATE:
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new DropAreaState()));
    return DefWindowProcW(window, message, wParam, lParam);

  case WM_SETFONT:
    if (state) {
      state->font = reinterpret_cast<HFONT>(wParam);
      if (LOWORD(lParam)) {
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    break;

  case WM_GETFONT:
    return reinterpret_cast<LRESULT>(state ? state->font : nullptr);

  case WM_SETTEXT: {
    const LRESULT result = DefWindowProcW(window, message, wParam, lParam);
    InvalidateRect(window, nullptr, FALSE);
    return result;
  }

  case WM_SIZE:
    InvalidateRect(window, nullptr, FALSE);
    return 0;

  case WM_ERASEBKGND:
    return 1;

  case WM_PAINT:
    if (state) {
      drawDropArea(window, *state);
      return 0;
    }
    break;

  case WM_NCDESTROY:
    delete state;
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    return 0;
  }

  return DefWindowProcW(window, message, wParam, lParam);
}

std::wstring widenUtf8(const std::string& value) {
  if (value.empty()) {
    return {};
  }

  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
  return result;
}

std::string narrowUtf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }

  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
  return result;
}

std::string narrowUtf8(const std::filesystem::path& path) {
  return narrowUtf8(path.wstring());
}

std::string jsonEscape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += ch;
      break;
    }
  }
  return escaped;
}

std::string jsonUnescape(const std::string& value) {
  std::string unescaped;
  unescaped.reserve(value.size());
  bool escaping = false;
  for (const char ch : value) {
    if (escaping) {
      switch (ch) {
      case 'n':
        unescaped += '\n';
        break;
      case 'r':
        unescaped += '\r';
        break;
      case 't':
        unescaped += '\t';
        break;
      default:
        unescaped += ch;
        break;
      }
      escaping = false;
      continue;
    }
    if (ch == '\\') {
      escaping = true;
      continue;
    }
    unescaped += ch;
  }
  return unescaped;
}

std::optional<std::string> valueAfterToken(const std::string& value, const std::string& token) {
  const auto start = value.find(token);
  if (start == std::string::npos) {
    return std::nullopt;
  }

  auto end = value.find(" | ", start + token.size());
  if (end == std::string::npos) {
    end = value.size();
  }

  return value.substr(start + token.size(), end - (start + token.size()));
}

SourceDisplay sourceDisplay(const std::string& source) {
  if (source == "Geländemodell points") {
    return SourceDisplay{L"Geländemodell points", L"", L"Detected"};
  }

  if (source.rfind("Block reference", 0) == 0) {
    return SourceDisplay{
        L"Block reference",
        widenUtf8(valueAfterToken(source, "block=").value_or("")),
        widenUtf8(valueAfterToken(source, "layer=").value_or("")),
    };
  }

  if (source.rfind("Layer: ", 0) == 0) {
    const auto layer = source.substr(7);
    return SourceDisplay{
        widenUtf8(layer),
        L"",
        widenUtf8(layer),
    };
  }

  return SourceDisplay{widenUtf8(source), L"", L""};
}

const char* severityText(dgm2xyz::DiagnosticSeverity severity) {
  switch (severity) {
  case dgm2xyz::DiagnosticSeverity::Info:
    return "INFO";
  case dgm2xyz::DiagnosticSeverity::Warning:
    return "WARNING";
  case dgm2xyz::DiagnosticSeverity::Error:
    return "ERROR";
  }
  return "INFO";
}

std::string timestampText() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm localTime{};
  localtime_s(&localTime, &time);

  std::ostringstream stream;
  stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
  return stream.str();
}

void logLine(const std::string& message) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  if (gLogPath.empty()) {
    return;
  }

  std::ofstream log(gLogPath, std::ios::app | std::ios::binary);
  log << timestampText() << " " << message << '\n';
}

void logDiagnostic(const dgm2xyz::Diagnostic& diagnostic) {
  logLine(std::string("[") + severityText(diagnostic.severity) + "] " + diagnostic.message);
}

void logDiagnostics(const std::vector<dgm2xyz::Diagnostic>& diagnostics) {
  for (const auto& diagnostic : diagnostics) {
    logDiagnostic(diagnostic);
  }
}

void logConversionResult(const dgm2xyz::ConversionResult& result) {
  logLine("Export finished: " + narrowUtf8(result.inputPath) + " -> " + narrowUtf8(result.outputPath) + " | " + result.summary());
  logDiagnostics(result.diagnostics);
}

std::filesystem::path executablePath() {
  std::wstring buffer(MAX_PATH, L'\0');
  DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  while (size == buffer.size()) {
    buffer.resize(buffer.size() * 2, L'\0');
    size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  }
  buffer.resize(size);
  return buffer;
}

void initializeLogging() {
  gLogPath = executablePath().parent_path() / "dgm2xyz.log";
  {
    std::lock_guard<std::mutex> lock(gLogMutex);
    std::ofstream log(gLogPath, std::ios::out | std::ios::binary);
    log << "\xEF\xBB\xBF";
    log << timestampText() << " dgm2xyz started\n";
  }
  logLine("Log file: " + narrowUtf8(gLogPath));
}

std::filesystem::path localAppDataPath() {
  PWSTR folder = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &folder))) {
    return executablePath().parent_path();
  }

  std::filesystem::path path(folder);
  CoTaskMemFree(folder);
  return path;
}

std::optional<int> jsonIntValue(const std::string& text, const std::string& key) {
  const auto keyPosition = text.find('"' + key + '"');
  if (keyPosition == std::string::npos) {
    return std::nullopt;
  }
  const auto colon = text.find(':', keyPosition);
  if (colon == std::string::npos) {
    return std::nullopt;
  }
  const auto start = text.find_first_of("-0123456789", colon + 1);
  if (start == std::string::npos) {
    return std::nullopt;
  }
  const auto end = text.find_first_not_of("0123456789-", start);
  try {
    return std::stoi(text.substr(start, end - start));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<double> jsonDoubleValue(const std::string& text, const std::string& key) {
  const auto keyPosition = text.find('"' + key + '"');
  if (keyPosition == std::string::npos) {
    return std::nullopt;
  }
  const auto colon = text.find(':', keyPosition);
  if (colon == std::string::npos) {
    return std::nullopt;
  }
  const auto start = text.find_first_of("-0123456789.", colon + 1);
  if (start == std::string::npos) {
    return std::nullopt;
  }
  const auto end = text.find_first_not_of("0123456789.-", start);
  try {
    return std::stod(text.substr(start, end - start));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<bool> jsonBoolValue(const std::string& text, const std::string& key) {
  const auto keyPosition = text.find('"' + key + '"');
  if (keyPosition == std::string::npos) {
    return std::nullopt;
  }
  const auto colon = text.find(':', keyPosition);
  if (colon == std::string::npos) {
    return std::nullopt;
  }
  const auto start = text.find_first_not_of(" \t\r\n", colon + 1);
  if (start == std::string::npos) {
    return std::nullopt;
  }
  if (text.compare(start, 4, "true") == 0) {
    return true;
  }
  if (text.compare(start, 5, "false") == 0) {
    return false;
  }
  return std::nullopt;
}

std::optional<std::string> jsonStringValue(const std::string& text, const std::string& key) {
  const auto keyPosition = text.find('"' + key + '"');
  if (keyPosition == std::string::npos) {
    return std::nullopt;
  }
  const auto colon = text.find(':', keyPosition);
  if (colon == std::string::npos) {
    return std::nullopt;
  }
  const auto quote = text.find('"', colon + 1);
  if (quote == std::string::npos) {
    return std::nullopt;
  }

  std::string value;
  bool escaping = false;
  for (std::size_t index = quote + 1; index < text.size(); ++index) {
    const char ch = text[index];
    if (escaping) {
      value += '\\';
      value += ch;
      escaping = false;
      continue;
    }
    if (ch == '\\') {
      escaping = true;
      continue;
    }
    if (ch == '"') {
      return jsonUnescape(value);
    }
    value += ch;
  }

  return std::nullopt;
}

void loadSettings() {
  gSettingsPath = localAppDataPath() / "dgm2xyz" / "settings.json";
  std::ifstream input(gSettingsPath, std::ios::binary);
  if (!input) {
    logLine("Settings file not found, using defaults: " + narrowUtf8(gSettingsPath));
    return;
  }

  const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (const auto value = jsonIntValue(text, "x")) {
    gSettings.windowX = *value;
  }
  if (const auto value = jsonIntValue(text, "y")) {
    gSettings.windowY = *value;
  }
  if (const auto value = jsonIntValue(text, "width")) {
    gSettings.windowWidth = std::clamp(*value, 640, 3840);
  }
  if (const auto value = jsonIntValue(text, "height")) {
    gSettings.windowHeight = std::clamp(*value, 480, 2160);
  }
  if (const auto value = jsonBoolValue(text, "maximized")) {
    gSettings.maximized = *value;
  }
  if (const auto value = jsonDoubleValue(text, "listSplitRatio")) {
    gSettings.listSplitRatio = std::clamp(*value, 0.18, 0.82);
  }
  if (const auto value = jsonDoubleValue(text, "previewHeightRatio")) {
    gSettings.previewHeightRatio = std::clamp(*value, 0.18, 0.72);
  }
  if (const auto value = jsonStringValue(text, "lastOpenDirectory"); value && !value->empty()) {
    gSettings.lastOpenDirectory = widenUtf8(*value);
  }
  logLine("Loaded settings: " + narrowUtf8(gSettingsPath));
}

void captureWindowSettings(HWND window, AppSettings& settings) {
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  if (!GetWindowPlacement(window, &placement)) {
    return;
  }

  settings.maximized = placement.showCmd == SW_SHOWMAXIMIZED;
  const RECT rect = placement.rcNormalPosition;
  settings.windowX = rect.left;
  settings.windowY = rect.top;
  settings.windowWidth = std::max(640L, rect.right - rect.left);
  settings.windowHeight = std::max(480L, rect.bottom - rect.top);
}

void saveSettings(HWND window, const AppState* state) {
  if (gSettingsPath.empty()) {
    return;
  }

  captureWindowSettings(window, gSettings);
  if (state) {
    gSettings.listSplitRatio = state->listSplitRatio;
    gSettings.previewHeightRatio = state->previewHeightRatio;
  }

  std::error_code error;
  std::filesystem::create_directories(gSettingsPath.parent_path(), error);
  if (error) {
    logLine("Could not create settings directory: " + error.message());
    return;
  }

  std::ofstream output(gSettingsPath, std::ios::binary | std::ios::trunc);
  if (!output) {
    logLine("Could not write settings file: " + narrowUtf8(gSettingsPath));
    return;
  }

  output << "{\n";
  output << "  \"window\": {\n";
  output << "    \"x\": " << gSettings.windowX << ",\n";
  output << "    \"y\": " << gSettings.windowY << ",\n";
  output << "    \"width\": " << gSettings.windowWidth << ",\n";
  output << "    \"height\": " << gSettings.windowHeight << ",\n";
  output << "    \"maximized\": " << (gSettings.maximized ? "true" : "false") << "\n";
  output << "  },\n";
  output << "  \"layout\": {\n";
  output << "    \"listSplitRatio\": " << std::fixed << std::setprecision(4) << gSettings.listSplitRatio << ",\n";
  output << "    \"previewHeightRatio\": " << std::fixed << std::setprecision(4) << gSettings.previewHeightRatio << "\n";
  output << "  },\n";
  output << "  \"files\": {\n";
  output << "    \"lastOpenDirectory\": \"" << jsonEscape(narrowUtf8(gSettings.lastOpenDirectory)) << "\"\n";
  output << "  }\n";
  output << "}\n";
  logLine("Saved settings: " + narrowUtf8(gSettingsPath));
}

void setWindowText(HWND handle, const std::wstring& text) {
  SetWindowTextW(handle, text.c_str());
}

void setListText(HWND listView, int row, int column, const std::wstring& text) {
  LVITEMW item{};
  item.mask = LVIF_TEXT;
  item.iItem = row;
  item.iSubItem = column;
  item.pszText = const_cast<wchar_t*>(text.c_str());
  ListView_SetItem(listView, &item);
}

void addColumn(HWND listView, int index, const wchar_t* text, int width) {
  LVCOLUMNW column{};
  column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  column.pszText = const_cast<wchar_t*>(text);
  column.cx = width;
  column.iSubItem = index;
  ListView_InsertColumn(listView, index, &column);
}

void clearList(HWND listView) {
  ListView_DeleteAllItems(listView);
}

void clearListSelection(HWND listView) {
  const int rowCount = ListView_GetItemCount(listView);
  for (int row = 0; row < rowCount; ++row) {
    ListView_SetItemState(listView, row, 0, LVIS_SELECTED | LVIS_FOCUSED);
  }
}

int measuredControlTextWidth(HWND control, HFONT font, int minWidth, int horizontalPadding) {
  const int length = GetWindowTextLengthW(control);
  std::wstring text(static_cast<std::size_t>(std::max(0, length)), L'\0');
  if (length > 0) {
    GetWindowTextW(control, text.data(), length + 1);
  }

  HDC dc = GetDC(control);
  if (!dc) {
    return minWidth;
  }

  HGDIOBJ oldFont = nullptr;
  if (font) {
    oldFont = SelectObject(dc, font);
  }

  SIZE size{};
  GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
  if (oldFont) {
    SelectObject(dc, oldFont);
  }
  ReleaseDC(control, dc);
  return std::max(minWidth, static_cast<int>(size.cx) + horizontalPadding);
}

std::map<std::string, std::size_t> pointTypeCounts(const std::vector<dgm2xyz::Point>& points) {
  std::map<std::string, std::size_t> counts;
  for (const auto& point : points) {
    counts[point.source]++;
  }
  return counts;
}

struct DgmDisplay {
  std::wstring name;
  std::wstring block;
  std::wstring layer;
  std::size_t triangleCount = 0;
};

std::map<std::string, DgmDisplay> dgmDisplays(const std::vector<dgm2xyz::PreviewTriangle>& triangles) {
  std::map<std::string, DgmDisplay> displays;
  for (const auto& triangle : triangles) {
    const auto source = triangle.source.empty() ? std::string("DGM") : triangle.source;
    auto& display = displays[source];
    display.triangleCount++;
    if (display.name.empty()) {
      const auto block = valueAfterToken(source, "block=").value_or(triangle.block);
      const auto layer = valueAfterToken(source, "layer=").value_or(triangle.layer);
      if (!block.empty()) {
        display.name = L"DGM " + widenUtf8(block);
      } else if (!layer.empty()) {
        display.name = L"DGM " + widenUtf8(layer);
      } else {
        display.name = widenUtf8(source);
      }
      display.block = widenUtf8(block);
      display.layer = widenUtf8(layer);
    }
  }
  return displays;
}

void logPointTypeCounts(const std::vector<dgm2xyz::Point>& points) {
  const auto counts = pointTypeCounts(points);
  if (counts.empty()) {
    logLine("Preview groups: none");
    return;
  }

  logLine("Preview groups:");
  for (const auto& [source, count] : counts) {
    logLine("  " + source + " = " + std::to_string(count));
  }
}

std::set<std::string> selectedDgmSources(HWND typeList);
std::vector<dgm2xyz::Point> exportPointsForDgms(const dgm2xyz::DrawingPreview& preview,
                                                const std::set<std::string>& selectedDgms);
void updateTextPreview(AppState& state);
void updateExportButton(AppState& state, std::size_t exportablePointCount = 0);

void updatePreviewViewForSelection(AppState& state) {
  if (!state.hasFile) {
    setPreviewData(state.previewView, dgm2xyz::DrawingPreview{}, {});
    setWindowText(state.textPreview, L"");
    return;
  }

  const auto& preview = state.file;
  if (!preview.loaded || preview.readResult.hasErrors()) {
    setPreviewData(state.previewView, dgm2xyz::DrawingPreview{}, {});
    setWindowText(state.textPreview, L"");
    return;
  }

  setPreviewData(state.previewView, preview.drawingPreview, selectedDgmSources(state.typeList));
  updateTextPreview(state);
}

void populateTypeList(AppState& state) {
  clearList(state.typeList);
  updateExportButton(state);

  if (!state.hasFile) {
    setWindowText(state.statusLabel, L"Drop or select a CAD file to preview DGMs.");
    setPreviewData(state.previewView, dgm2xyz::DrawingPreview{}, {});
    setWindowText(state.textPreview, L"");
    return;
  }

  const auto& preview = state.file;
  if (!preview.loaded) {
    setWindowText(state.statusLabel, L"Reading DGMs...");
    setPreviewData(state.previewView, dgm2xyz::DrawingPreview{}, {});
    setWindowText(state.textPreview, L"");
    return;
  }

  if (preview.readResult.hasErrors()) {
    setWindowText(state.statusLabel, L"Preview failed. See log for details.");
    setPreviewData(state.previewView, dgm2xyz::DrawingPreview{}, {});
    setWindowText(state.textPreview, L"");
    return;
  }

  const auto dgms = dgmDisplays(preview.drawingPreview.triangles);
  if (dgms.empty()) {
    setWindowText(state.statusLabel, L"No DGM 3D-face blocks found.");
    setPreviewData(state.previewView, preview.drawingPreview, {});
    setWindowText(state.textPreview, L"");
    return;
  }

  int row = 0;
  for (const auto& [source, display] : dgms) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = const_cast<wchar_t*>(display.name.c_str());
    const int inserted = ListView_InsertItem(state.typeList, &item);
    setListText(state.typeList, inserted, 1, display.block);
    setListText(state.typeList, inserted, 2, display.layer);
    setListText(state.typeList, inserted, 3, std::to_wstring(display.triangleCount));
    setListText(state.typeList, inserted, 4, widenUtf8(source));
    ListView_SetCheckState(state.typeList, inserted, TRUE);
    ++row;
  }

  setWindowText(state.statusLabel, L"Choose DGM blocks, then write XYZ.");
  updatePreviewViewForSelection(state);
}

std::set<std::string> selectedDgmSources(HWND typeList) {
  std::set<std::string> selected;
  const int rowCount = ListView_GetItemCount(typeList);
  for (int row = 0; row < rowCount; ++row) {
    if (!ListView_GetCheckState(typeList, row)) {
      continue;
    }
    wchar_t buffer[512]{};
    ListView_GetItemText(typeList, row, 4, buffer, static_cast<int>(std::size(buffer)));
    selected.insert(narrowUtf8(std::wstring(buffer)));
  }
  return selected;
}

std::string pointKey(const dgm2xyz::PreviewVertex& vertex) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(3) << vertex.x << '|' << vertex.y << '|' << vertex.z;
  return stream.str();
}

std::vector<dgm2xyz::Point> exportPointsForDgms(const dgm2xyz::DrawingPreview& preview,
                                                const std::set<std::string>& selectedDgms) {
  std::vector<dgm2xyz::Point> points;
  std::set<std::string> seen;
  const auto addVertex = [&](const dgm2xyz::PreviewVertex& vertex, const std::string& source) {
    if (seen.insert(pointKey(vertex)).second) {
      points.push_back(dgm2xyz::Point{vertex.x, vertex.y, vertex.z, source});
    }
  };

  for (const auto& triangle : preview.triangles) {
    if (!selectedDgms.contains(triangle.source)) {
      continue;
    }
    addVertex(triangle.a, triangle.source);
    addVertex(triangle.b, triangle.source);
    addVertex(triangle.c, triangle.source);
  }

  return points;
}

std::wstring xyzPreviewText(const std::vector<dgm2xyz::Point>& points) {
  constexpr std::size_t kPreviewLimit = 20000;
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  stream << std::fixed << std::setprecision(3);
  const std::size_t count = std::min(points.size(), kPreviewLimit);
  for (std::size_t index = 0; index < count; ++index) {
    stream << points[index].x << ' ' << points[index].y << ' ' << points[index].z << "\r\n";
  }
  if (points.size() > kPreviewLimit) {
    stream << "... preview truncated after " << kPreviewLimit << " of " << points.size() << " points\r\n";
  }
  return widenUtf8(stream.str());
}

void updateExportButton(AppState& state, std::size_t exportablePointCount) {
  EnableWindow(state.exportButton,
               state.hasFile && state.file.loaded && !state.file.readResult.hasErrors() && exportablePointCount > 0);
  EnableWindow(state.outputEdit, state.hasFile);
}

void updateTextPreview(AppState& state) {
  if (!state.hasFile || !state.file.loaded || state.file.readResult.hasErrors()) {
    setWindowText(state.textPreview, L"");
    updateExportButton(state);
    return;
  }

  const auto sources = selectedDgmSources(state.typeList);
  const auto points = exportPointsForDgms(state.file.drawingPreview, sources);
  setWindowText(state.textPreview, xyzPreviewText(points));
  updateExportButton(state, points.size());
}

void startPreview(HWND window, std::filesystem::path path) {
  logLine("Preview queued: " + narrowUtf8(path));
  std::thread([window, path = std::move(path)] {
    logLine("Preview started: " + narrowUtf8(path));
    auto finished = std::make_unique<FinishedPreview>();
    finished->path = path;
    finished->preview = AppPointReader{}.readPreview(path);
    logLine("Preview finished: " + narrowUtf8(path) + " | " + std::to_string(finished->preview.points.size()) +
            " point(s), " + std::to_string(finished->preview.lines.size()) + " line(s), " +
            std::to_string(finished->preview.polylines.size()) + " polyline(s)");
    logPointTypeCounts(finished->preview.points);
    logDiagnostics(finished->preview.diagnostics);
    PostMessageW(window, kPreviewDoneMessage, 0, reinterpret_cast<LPARAM>(finished.release()));
  }).detach();
}

void loadFile(HWND window, const std::filesystem::path& path) {
  const auto state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state) {
    return;
  }

  state->file = FilePreview{path};
  state->hasFile = true;
  setWindowText(state->outputEdit, dgm2xyz::defaultOutputPath(path).filename().wstring());
  updateExportButton(*state);
  populateTypeList(*state);
  setWindowText(state->statusLabel, (L"Reading " + path.filename().wstring() + L"...").c_str());
  startPreview(window, path);
}

void loadFirstFile(HWND window, const std::vector<std::filesystem::path>& paths) {
  if (!paths.empty()) {
    loadFile(window, paths.front());
  }
}

std::filesystem::path outputPathFromEdit(const AppState& state) {
  const int length = GetWindowTextLengthW(state.outputEdit);
  std::wstring text(static_cast<std::size_t>(std::max(0, length)), L'\0');
  if (length > 0) {
    GetWindowTextW(state.outputEdit, text.data(), length + 1);
  }

  std::filesystem::path outputPath = text.empty() ? dgm2xyz::defaultOutputPath(state.file.path) : std::filesystem::path(text);
  if (outputPath.extension().empty()) {
    outputPath.replace_extension(L".xyz");
  }
  if (outputPath.is_relative()) {
    outputPath = state.file.path.parent_path() / outputPath;
  }
  return outputPath;
}

void startExport(HWND window) {
  const auto state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state) {
    return;
  }

  if (!state->hasFile || !state->file.loaded) {
    setWindowText(state->statusLabel, L"Select a file before exporting.");
    return;
  }

  const auto sources = selectedDgmSources(state->typeList);
  if (sources.empty()) {
    setWindowText(state->statusLabel, L"Check at least one DGM.");
    return;
  }
  const auto exportPoints = exportPointsForDgms(state->file.drawingPreview, sources);
  if (exportPoints.empty()) {
    setWindowText(state->statusLabel, L"Checked DGMs have no exportable triangle vertices.");
    return;
  }

  auto preview = state->file;
  const auto outputPath = outputPathFromEdit(*state);
  setWindowText(state->statusLabel, L"Writing XYZ...");
  EnableWindow(state->exportButton, FALSE);

  std::thread([window, preview = std::move(preview), sources, outputPath, exportPoints = std::move(exportPoints)] {
    auto finished = std::make_unique<FinishedExport>();
    finished->result.inputPath = preview.path;
    finished->result.outputPath = outputPath;
    try {
      dgm2xyz::writeXyzFile(outputPath, exportPoints);
      finished->result.pointCount = exportPoints.size();
      finished->result.status = dgm2xyz::ConversionStatus::Succeeded;
    } catch (const std::exception& exception) {
      finished->result.status = dgm2xyz::ConversionStatus::Failed;
      finished->result.diagnostics.push_back({dgm2xyz::DiagnosticSeverity::Error, exception.what()});
    }
    logConversionResult(finished->result);
    PostMessageW(window, kExportDoneMessage, 0, reinterpret_cast<LPARAM>(finished.release()));
  }).detach();
}

std::vector<std::filesystem::path> pathsFromDrop(HDROP drop) {
  std::vector<std::filesystem::path> paths;
  const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  for (UINT index = 0; index < count; ++index) {
    const UINT length = DragQueryFileW(drop, index, nullptr, 0);
    std::wstring path(length + 1, L'\0');
    DragQueryFileW(drop, index, path.data(), length + 1);
    path.resize(length);
    paths.emplace_back(path);
  }
  return paths;
}

std::vector<std::filesystem::path> pickFiles(HWND owner) {
  std::vector<wchar_t> buffer(32768, L'\0');

  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = L"CAD files (*.dxf;*.dwg)\0*.dxf;*.dwg\0DXF files (*.dxf)\0*.dxf\0DWG files (*.dwg)\0*.dwg\0All files (*.*)\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST;
  const auto initialDirectory = gSettings.lastOpenDirectory.wstring();
  if (!initialDirectory.empty()) {
    dialog.lpstrInitialDir = initialDirectory.c_str();
  }

  if (!GetOpenFileNameW(&dialog)) {
    return {};
  }

  const wchar_t* cursor = buffer.data();
  std::filesystem::path first(cursor);
  gSettings.lastOpenDirectory = first.parent_path();
  return {first};
}

void layoutControls(HWND window, AppState& state, bool redrawChildren = true) {
  RECT rect{};
  GetClientRect(window, &rect);
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  const int margin = scaleForDpi(12, state.currentDpi);
  const int splitterSize = scaleForDpi(6, state.currentDpi);
  const int buttonHeight = scaleForDpi(30, state.currentDpi);
  const int pickButtonWidth = scaleForDpi(130, state.currentDpi);
  const int buttonGap = scaleForDpi(10, state.currentDpi);
  const int exportButtonWidth = scaleForDpi(76, state.currentDpi);
  const int outputEditLeft = margin + pickButtonWidth + buttonGap + exportButtonWidth + buttonGap;
  const int outputEditWidth = std::max(scaleForDpi(120, state.currentDpi), width - outputEditLeft - margin);
  const int dropHeight = scaleForDpi(64, state.currentDpi);
  const int statusHeight = scaleForDpi(24, state.currentDpi);
  const int top = margin + buttonHeight + scaleForDpi(10, state.currentDpi) + dropHeight + scaleForDpi(10, state.currentDpi);
  const int contentWidth = std::max(1, width - margin * 2);
  const int contentHeight = std::max(1, height - top - statusHeight - margin * 2);
  const int minListHeight = scaleForDpi(90, state.currentDpi);
  const int minPreviewHeight = scaleForDpi(130, state.currentDpi);
  const int adjustableHeight = std::max(1, contentHeight - splitterSize);
  int previewHeight = static_cast<int>(static_cast<double>(adjustableHeight) * state.previewHeightRatio);
  previewHeight = std::clamp(previewHeight, minPreviewHeight, std::max(minPreviewHeight, adjustableHeight - minListHeight));
  const int listHeight = std::max(minListHeight, adjustableHeight - previewHeight);
  const int previewTop = top + listHeight + splitterSize;

  const int adjustableWidth = std::max(1, contentWidth - splitterSize);
  const int minLeftWidth = std::min(scaleForDpi(220, state.currentDpi), adjustableWidth);
  const int minRightWidth = std::min(scaleForDpi(260, state.currentDpi), adjustableWidth);
  int leftWidth = static_cast<int>(static_cast<double>(adjustableWidth) * state.listSplitRatio);
  if (adjustableWidth > minLeftWidth + minRightWidth) {
    leftWidth = std::clamp(leftWidth, minLeftWidth, adjustableWidth - minRightWidth);
  } else {
    leftWidth = adjustableWidth / 2;
  }
  const int rightWidth = std::max(1, adjustableWidth - leftWidth);
  const int verticalSplitterLeft = margin + leftWidth;
  const int textPreviewLeft = verticalSplitterLeft + splitterSize;

  state.verticalSplitter = RECT{verticalSplitterLeft, top, verticalSplitterLeft + splitterSize, top + listHeight};
  state.horizontalSplitter = RECT{margin, top + listHeight, margin + contentWidth, top + listHeight + splitterSize};

  HDWP defer = BeginDeferWindowPos(8);
  if (defer) {
    constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    defer = DeferWindowPos(defer, state.pickButton, nullptr, margin, margin, pickButtonWidth, buttonHeight, flags);
    defer = DeferWindowPos(defer, state.exportButton, nullptr, margin + pickButtonWidth + buttonGap, margin, exportButtonWidth, buttonHeight, flags);
    defer = DeferWindowPos(defer, state.outputEdit, nullptr, outputEditLeft, margin, outputEditWidth, buttonHeight, flags);
    defer = DeferWindowPos(defer, state.dropLabel, nullptr, margin, margin + buttonHeight + scaleForDpi(10, state.currentDpi), width - margin * 2, dropHeight, flags);
    defer = DeferWindowPos(defer, state.typeList, nullptr, margin, top, leftWidth, listHeight, flags);
    defer = DeferWindowPos(defer, state.textPreview, nullptr, textPreviewLeft, top, rightWidth, listHeight, flags);
    defer = DeferWindowPos(defer, state.previewView, nullptr, margin, previewTop, width - margin * 2, previewHeight, flags);
    defer = DeferWindowPos(defer, state.statusLabel, nullptr, margin, height - statusHeight - margin, width - margin * 2, statusHeight, flags);
    EndDeferWindowPos(defer);
  }

  ListView_SetColumnWidth(state.typeList, 0, scaleForDpi(145, state.currentDpi));
  ListView_SetColumnWidth(state.typeList, 1, scaleForDpi(95, state.currentDpi));
  ListView_SetColumnWidth(state.typeList, 2, std::max(scaleForDpi(90, state.currentDpi), leftWidth - scaleForDpi(315, state.currentDpi)));
  ListView_SetColumnWidth(state.typeList, 3, scaleForDpi(70, state.currentDpi));
  ListView_SetColumnWidth(state.typeList, 4, 0);
  InvalidateRect(window, &state.verticalSplitter, FALSE);
  InvalidateRect(window, &state.horizontalSplitter, FALSE);
}

bool pointInRect(const RECT& rect, POINT point) {
  return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

SplitterDrag splitterAtPoint(const AppState& state, POINT point) {
  if (pointInRect(state.verticalSplitter, point)) {
    return SplitterDrag::VerticalLists;
  }
  if (pointInRect(state.horizontalSplitter, point)) {
    return SplitterDrag::HorizontalPreview;
  }
  return SplitterDrag::None;
}

void drawSplitters(HWND window, AppState& state) {
  PAINTSTRUCT paint{};
  HDC targetDc = BeginPaint(window, &paint);
  HDC dc = targetDc;
  HPAINTBUFFER buffer = BeginBufferedPaint(targetDc, &paint.rcPaint, BPBF_COMPATIBLEBITMAP, nullptr, &dc);
  if (!buffer || !dc) {
    dc = targetDc;
  }

  FillRect(dc, &paint.rcPaint, GetSysColorBrush(COLOR_WINDOW));
  const HBRUSH faceBrush = GetSysColorBrush(COLOR_BTNFACE);
  const HBRUSH shadowBrush = GetSysColorBrush(COLOR_3DSHADOW);
  FillRect(dc, &state.verticalSplitter, faceBrush);
  FillRect(dc, &state.horizontalSplitter, faceBrush);

  RECT verticalLine = state.verticalSplitter;
  verticalLine.left += (verticalLine.right - verticalLine.left) / 2;
  verticalLine.right = verticalLine.left + 1;
  FillRect(dc, &verticalLine, shadowBrush);

  RECT horizontalLine = state.horizontalSplitter;
  horizontalLine.top += (horizontalLine.bottom - horizontalLine.top) / 2;
  horizontalLine.bottom = horizontalLine.top + 1;
  FillRect(dc, &horizontalLine, shadowBrush);
  if (buffer) {
    EndBufferedPaint(buffer, TRUE);
  }
  EndPaint(window, &paint);
}

void updateSplitterDrag(HWND window, AppState& state, POINT point) {
  RECT rect{};
  GetClientRect(window, &rect);
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  const int margin = scaleForDpi(12, state.currentDpi);
  const int splitterSize = scaleForDpi(6, state.currentDpi);
  const int buttonHeight = scaleForDpi(30, state.currentDpi);
  const int dropHeight = scaleForDpi(64, state.currentDpi);
  const int statusHeight = scaleForDpi(24, state.currentDpi);
  const int top = margin + buttonHeight + scaleForDpi(10, state.currentDpi) + dropHeight + scaleForDpi(10, state.currentDpi);

  if (state.splitterDrag == SplitterDrag::VerticalLists) {
    const int contentWidth = std::max(1, width - margin * 2);
    const int adjustableWidth = std::max(1, contentWidth - splitterSize);
    state.listSplitRatio = std::clamp(static_cast<double>(point.x - margin) / static_cast<double>(adjustableWidth), 0.18, 0.82);
    layoutControls(window, state, false);
    return;
  }

  if (state.splitterDrag == SplitterDrag::HorizontalPreview) {
    const int contentHeight = std::max(1, height - top - statusHeight - margin * 2);
    const int adjustableHeight = std::max(1, contentHeight - splitterSize);
    const int listHeight = adjustableHeight > scaleForDpi(160, state.currentDpi)
                               ? std::clamp(static_cast<int>(point.y) - top, scaleForDpi(70, state.currentDpi), adjustableHeight - scaleForDpi(90, state.currentDpi))
                               : adjustableHeight / 2;
    const int previewHeight = std::max(scaleForDpi(90, state.currentDpi), adjustableHeight - listHeight);
    state.previewHeightRatio = std::clamp(static_cast<double>(previewHeight) / static_cast<double>(adjustableHeight), 0.18, 0.72);
    layoutControls(window, state, false);
  }
}

HWND createChild(HWND parent, const wchar_t* className, const wchar_t* text, DWORD style, DWORD exStyle, int id) {
  return CreateWindowExW(exStyle,
                         className,
                         text,
                         WS_CHILD | WS_VISIBLE | style,
                         0,
                         0,
                         0,
                         0,
                         parent,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         nullptr,
                         nullptr);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  auto state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

  switch (message) {
  case WM_CREATE: {
    auto ownedState = std::make_unique<AppState>();
    ownedState->currentDpi = windowDpi(window);
    ownedState->uiFont = createUiFont(ownedState->currentDpi);
    ownedState->listSplitRatio = gSettings.listSplitRatio;
    ownedState->previewHeightRatio = gSettings.previewHeightRatio;

    ownedState->pickButton = createChild(window, L"BUTTON", L"Select file...", BS_PUSHBUTTON, 0, kPickButtonId);
    ownedState->exportButton = createChild(window, L"BUTTON", L"Write", BS_PUSHBUTTON, 0, kExportButtonId);
    ownedState->outputEdit = createChild(window, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, kOutputEditId);
    ownedState->dropLabel = createChild(window, kDropAreaClassName, L"Drop DXF/DWG files here", 0, 0, kDropLabelId);
    ownedState->typeList = createChild(window, WC_LISTVIEWW, L"", LVS_REPORT, WS_EX_CLIENTEDGE, kTypeListId);
    ownedState->textPreview = createChild(window,
                                          L"EDIT",
                                          L"",
                                          ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,
                                          WS_EX_CLIENTEDGE,
                                          kTextPreviewId);
    ownedState->previewView = createChild(window, kPreviewClassName, L"", 0, 0, kPreviewViewId);
    ownedState->statusLabel = createChild(window, L"STATIC", L"Drop or select a CAD file to preview DGMs.", SS_LEFT | SS_CENTERIMAGE, 0, kStatusLabelId);

    applyUiFont(*ownedState);

    SetWindowTheme(ownedState->typeList, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyleEx(ownedState->typeList,
                                        LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES,
                                        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
    addColumn(ownedState->typeList, 0, L"DGM", 150);
    addColumn(ownedState->typeList, 1, L"Block", 110);
    addColumn(ownedState->typeList, 2, L"Layer", 220);
    addColumn(ownedState->typeList, 3, L"Triangles", 70);
    addColumn(ownedState->typeList, 4, L"Source", 0);
    EnableWindow(ownedState->exportButton, FALSE);

    DragAcceptFiles(window, TRUE);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ownedState.release()));
    layoutControls(window, *reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA)));
    return 0;
  }

  case WM_SIZE:
    if (state) {
      layoutControls(window, *state);
    }
    return 0;

  case WM_GETMINMAXINFO: {
    auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
    const UINT dpi = state ? state->currentDpi : windowDpi(window);
    info->ptMinTrackSize.x = scaleForDpi(620, dpi);
    info->ptMinTrackSize.y = scaleForDpi(420, dpi);
    return 0;
  }

  case WM_DPICHANGED:
    if (state) {
      recreateUiFont(*state, HIWORD(wParam));
      const auto* suggestedRect = reinterpret_cast<RECT*>(lParam);
      SetWindowPos(window,
                   nullptr,
                   suggestedRect->left,
                   suggestedRect->top,
                   suggestedRect->right - suggestedRect->left,
                   suggestedRect->bottom - suggestedRect->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      layoutControls(window, *state);
      return 0;
    }
    break;

  case WM_ERASEBKGND:
    return 1;

  case WM_PAINT:
    if (state) {
      drawSplitters(window, *state);
      return 0;
    }
    break;

  case WM_COMMAND:
    if (LOWORD(wParam) == kPickButtonId) {
      loadFirstFile(window, pickFiles(window));
    } else if (LOWORD(wParam) == kExportButtonId) {
      startExport(window);
    }
    return 0;

  case WM_NOTIFY:
    if (state && reinterpret_cast<NMHDR*>(lParam)->idFrom == kTypeListId &&
        reinterpret_cast<NMHDR*>(lParam)->code == LVN_ITEMCHANGED) {
      setPreviewSelection(state->previewView, selectedDgmSources(state->typeList));
      updateTextPreview(*state);
      clearListSelection(state->typeList);
    }
    return 0;

  case WM_DROPFILES: {
    const auto drop = reinterpret_cast<HDROP>(wParam);
    loadFirstFile(window, pathsFromDrop(drop));
    DragFinish(drop);
    return 0;
  }

  case WM_SETCURSOR:
    if (state && LOWORD(lParam) == HTCLIENT) {
      POINT point{};
      GetCursorPos(&point);
      ScreenToClient(window, &point);
      const auto splitter = splitterAtPoint(*state, point);
      if (splitter == SplitterDrag::VerticalLists) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return TRUE;
      }
      if (splitter == SplitterDrag::HorizontalPreview) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
        return TRUE;
      }
    }
    break;

  case WM_LBUTTONDOWN:
    if (state) {
      const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      state->splitterDrag = splitterAtPoint(*state, point);
      if (state->splitterDrag != SplitterDrag::None) {
        SetCapture(window);
        return 0;
      }
    }
    break;

  case WM_MOUSEMOVE:
    if (state && state->splitterDrag != SplitterDrag::None) {
      const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      updateSplitterDrag(window, *state, point);
      SetCursor(LoadCursorW(nullptr, state->splitterDrag == SplitterDrag::VerticalLists ? IDC_SIZEWE : IDC_SIZENS));
      return 0;
    }
    break;

  case WM_LBUTTONUP:
    if (state && state->splitterDrag != SplitterDrag::None) {
      state->splitterDrag = SplitterDrag::None;
      ReleaseCapture();
      return 0;
    }
    break;

  case kPreviewDoneMessage: {
    std::unique_ptr<FinishedPreview> finished(reinterpret_cast<FinishedPreview*>(lParam));
    if (state && finished && state->hasFile && finished->path == state->file.path) {
      state->file.path = std::move(finished->path);
      state->file.drawingPreview = std::move(finished->preview);
      state->file.readResult = state->file.drawingPreview.toReadResult();
      state->file.loaded = true;
      populateTypeList(*state);
    }
    return 0;
  }

  case kExportDoneMessage: {
    std::unique_ptr<FinishedExport> finished(reinterpret_cast<FinishedExport*>(lParam));
    if (state && finished) {
      setWindowText(state->statusLabel, widenUtf8(finished->result.summary()));
      updateTextPreview(*state);
      const bool succeeded = finished->result.status == dgm2xyz::ConversionStatus::Succeeded;
      MessageBoxW(window,
                  widenUtf8(finished->result.summary()).c_str(),
                  succeeded ? L"XYZ written" : L"XYZ export failed",
                  MB_OK | (succeeded ? MB_ICONINFORMATION : MB_ICONERROR));
    }
    return 0;
  }

  case WM_DESTROY:
    DragAcceptFiles(window, FALSE);
    saveSettings(window, state);
    if (state) {
      releaseUiFont(state->uiFont);
      state->uiFont = nullptr;
    }
    delete state;
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    PostQuitMessage(0);
    return 0;
  }

  return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  BufferedPaintInit();
  initializeLogging();
  loadSettings();

  INITCOMMONCONTROLSEX controls{};
  controls.dwSize = sizeof(controls);
  controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&controls);

  const wchar_t* className = L"dgm2xyz.MainWindow";

  WNDCLASSEXW previewClass{};
  previewClass.cbSize = sizeof(previewClass);
  previewClass.style = CS_DBLCLKS;
  previewClass.lpfnWndProc = previewWindowProc;
  previewClass.hInstance = instance;
  previewClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
  previewClass.hbrBackground = nullptr;
  previewClass.lpszClassName = kPreviewClassName;
  RegisterClassExW(&previewClass);

  WNDCLASSEXW dropAreaClass{};
  dropAreaClass.cbSize = sizeof(dropAreaClass);
  dropAreaClass.lpfnWndProc = dropAreaWindowProc;
  dropAreaClass.hInstance = instance;
  dropAreaClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  dropAreaClass.hbrBackground = nullptr;
  dropAreaClass.lpszClassName = kDropAreaClassName;
  RegisterClassExW(&dropAreaClass);

  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = windowProc;
  windowClass.hInstance = instance;
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  windowClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
  windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  windowClass.lpszClassName = className;

  RegisterClassExW(&windowClass);

  HWND window = CreateWindowExW(0,
                                className,
                                L"dgm2xyz",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                gSettings.windowX,
                                gSettings.windowY,
                                gSettings.windowWidth,
                                gSettings.windowHeight,
                                nullptr,
                                nullptr,
                                instance,
                                nullptr);

  if (!window) {
    BufferedPaintUnInit();
    return 1;
  }

  ShowWindow(window, gSettings.maximized ? SW_SHOWMAXIMIZED : showCommand);
  UpdateWindow(window);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  BufferedPaintUnInit();
  return static_cast<int>(message.wParam);
}
