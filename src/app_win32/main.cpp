#include "dgm2xyz/Conversion.h"
#include "dgm2xyz/DwgPointReader.h"
#include "dgm2xyz/DxfPointReader.h"

#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
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
constexpr int kFileListId = 1004;
constexpr int kTypeListId = 1005;
constexpr int kStatusLabelId = 1006;
constexpr UINT kPreviewDoneMessage = WM_APP + 1;
constexpr UINT kExportDoneMessage = WM_APP + 2;

std::mutex gLogMutex;
std::filesystem::path gLogPath;

struct FilePreview {
  std::filesystem::path path;
  dgm2xyz::ReadResult readResult;
  bool loaded = false;
};

struct AppState {
  HWND pickButton = nullptr;
  HWND exportButton = nullptr;
  HWND dropLabel = nullptr;
  HWND fileList = nullptr;
  HWND typeList = nullptr;
  HWND statusLabel = nullptr;
  HFONT uiFont = nullptr;
  std::vector<FilePreview> files;
};

struct FinishedPreview {
  int row = -1;
  std::filesystem::path path;
  dgm2xyz::ReadResult result;
};

struct FinishedExport {
  dgm2xyz::ConversionResult result;
};

class AppPointReader final : public dgm2xyz::CadPointReader {
public:
  dgm2xyz::ReadResult readPoints(const std::filesystem::path& file) const override {
    const auto extension = lowerExtension(file.extension().wstring());
    if (extension == L".dwg") {
      return dwgReader_.readPoints(file);
    }

    return dxfReader_.readPoints(file);
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
    log << timestampText() << " dgm2xyz started\n";
  }
  logLine("Log file: " + narrowUtf8(gLogPath));
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

int selectedFileIndex(HWND fileList) {
  return ListView_GetNextItem(fileList, -1, LVNI_SELECTED);
}

std::map<std::string, std::size_t> pointTypeCounts(const std::vector<dgm2xyz::Point>& points) {
  std::map<std::string, std::size_t> counts;
  for (const auto& point : points) {
    counts[point.source]++;
  }
  return counts;
}

void populateTypeList(AppState& state, int fileIndex) {
  clearList(state.typeList);
  EnableWindow(state.exportButton, FALSE);

  if (fileIndex < 0 || fileIndex >= static_cast<int>(state.files.size())) {
    setWindowText(state.statusLabel, L"Drop or select a CAD file to preview point types.");
    return;
  }

  const auto& preview = state.files[static_cast<std::size_t>(fileIndex)];
  if (!preview.loaded) {
    setWindowText(state.statusLabel, L"Reading point types...");
    return;
  }

  if (preview.readResult.hasErrors()) {
    setWindowText(state.statusLabel, L"Preview failed. See log for details.");
    return;
  }

  const auto counts = pointTypeCounts(preview.readResult.points);
  if (counts.empty()) {
    setWindowText(state.statusLabel, L"No supported point objects found.");
    return;
  }

  int row = 0;
  for (const auto& [source, count] : counts) {
    const auto sourceText = widenUtf8(source);
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = const_cast<wchar_t*>(sourceText.c_str());
    const int inserted = ListView_InsertItem(state.typeList, &item);
    setListText(state.typeList, inserted, 1, std::to_wstring(count));
    ListView_SetCheckState(state.typeList, inserted, TRUE);
    ++row;
  }

  EnableWindow(state.exportButton, TRUE);
  setWindowText(state.statusLabel, L"Choose point types, then write XYZ.");
}

void updateFileRow(HWND fileList, int row, const std::wstring& status, const std::wstring& result) {
  setListText(fileList, row, 1, status);
  setListText(fileList, row, 2, result);
}

int addFileRow(HWND fileList, const std::filesystem::path& path) {
  const auto pathText = path.wstring();

  LVITEMW item{};
  item.mask = LVIF_TEXT;
  item.iItem = ListView_GetItemCount(fileList);
  item.pszText = const_cast<wchar_t*>(pathText.c_str());
  const int row = ListView_InsertItem(fileList, &item);

  updateFileRow(fileList, row, L"Reading", L"");
  return row;
}

std::set<std::string> selectedSources(HWND typeList) {
  std::set<std::string> selected;
  const int count = ListView_GetItemCount(typeList);
  for (int row = 0; row < count; ++row) {
    if (!ListView_GetCheckState(typeList, row)) {
      continue;
    }

    wchar_t buffer[512]{};
    ListView_GetItemText(typeList, row, 0, buffer, static_cast<int>(std::size(buffer)));
    selected.insert(narrowUtf8(std::wstring(buffer)));
  }
  return selected;
}

void startPreview(HWND window, int row, std::filesystem::path path) {
  logLine("Preview queued: " + narrowUtf8(path));
  std::thread([window, row, path = std::move(path)] {
    logLine("Preview started: " + narrowUtf8(path));
    auto finished = std::make_unique<FinishedPreview>();
    finished->row = row;
    finished->path = path;
    finished->result = AppPointReader{}.readPoints(path);
    logLine("Preview finished: " + narrowUtf8(path) + " | " + std::to_string(finished->result.points.size()) + " point(s)");
    logDiagnostics(finished->result.diagnostics);
    PostMessageW(window, kPreviewDoneMessage, 0, reinterpret_cast<LPARAM>(finished.release()));
  }).detach();
}

void enqueueFiles(HWND window, const std::vector<std::filesystem::path>& paths) {
  const auto state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state || !state->fileList) {
    return;
  }

  for (const auto& path : paths) {
    const int row = addFileRow(state->fileList, path);
    state->files.push_back(FilePreview{path});
    startPreview(window, row, path);
  }

  if (selectedFileIndex(state->fileList) < 0 && !state->files.empty()) {
    ListView_SetItemState(state->fileList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  }
}

void startExport(HWND window) {
  const auto state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state) {
    return;
  }

  const int index = selectedFileIndex(state->fileList);
  if (index < 0 || index >= static_cast<int>(state->files.size())) {
    setWindowText(state->statusLabel, L"Select a file before exporting.");
    return;
  }

  const auto sources = selectedSources(state->typeList);
  if (sources.empty()) {
    setWindowText(state->statusLabel, L"Select at least one point type.");
    return;
  }

  auto preview = state->files[static_cast<std::size_t>(index)];
  updateFileRow(state->fileList, index, L"Writing", L"");
  setWindowText(state->statusLabel, L"Writing XYZ...");
  EnableWindow(state->exportButton, FALSE);

  std::thread([window, preview = std::move(preview), sources] {
    auto finished = std::make_unique<FinishedExport>();
    finished->result = dgm2xyz::exportPoints(preview.path, preview.readResult.points, sources);
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
  dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT;

  if (!GetOpenFileNameW(&dialog)) {
    return {};
  }

  std::vector<std::filesystem::path> paths;
  const wchar_t* cursor = buffer.data();
  std::filesystem::path first(cursor);
  cursor += wcslen(cursor) + 1;

  if (*cursor == L'\0') {
    paths.push_back(first);
    return paths;
  }

  while (*cursor != L'\0') {
    paths.push_back(first / cursor);
    cursor += wcslen(cursor) + 1;
  }

  return paths;
}

void layoutControls(HWND window, AppState& state) {
  RECT rect{};
  GetClientRect(window, &rect);
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  const int margin = 12;
  const int buttonHeight = 30;
  const int dropHeight = 64;
  const int statusHeight = 24;
  const int top = margin + buttonHeight + 10 + dropHeight + 10;
  const int listHeight = height - top - statusHeight - margin * 2;
  const int leftWidth = (width - margin * 3) / 2;
  const int rightWidth = width - margin * 3 - leftWidth;

  MoveWindow(state.pickButton, margin, margin, 150, buttonHeight, TRUE);
  MoveWindow(state.exportButton, margin + 160, margin, 130, buttonHeight, TRUE);
  MoveWindow(state.dropLabel, margin, margin + buttonHeight + 10, width - margin * 2, dropHeight, TRUE);
  MoveWindow(state.fileList, margin, top, leftWidth, listHeight, TRUE);
  MoveWindow(state.typeList, margin * 2 + leftWidth, top, rightWidth, listHeight, TRUE);
  MoveWindow(state.statusLabel, margin, height - statusHeight - margin, width - margin * 2, statusHeight, TRUE);

  ListView_SetColumnWidth(state.fileList, 0, leftWidth - 250);
  ListView_SetColumnWidth(state.fileList, 1, 85);
  ListView_SetColumnWidth(state.fileList, 2, 150);
  ListView_SetColumnWidth(state.typeList, 0, rightWidth - 100);
  ListView_SetColumnWidth(state.typeList, 1, 80);
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
    ownedState->uiFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    ownedState->pickButton = createChild(window, L"BUTTON", L"Select files...", BS_PUSHBUTTON, 0, kPickButtonId);
    ownedState->exportButton = createChild(window, L"BUTTON", L"Write XYZ", BS_PUSHBUTTON, 0, kExportButtonId);
    ownedState->dropLabel = createChild(window, L"STATIC", L"Drop DXF/DWG files here", SS_CENTER | SS_CENTERIMAGE, WS_EX_CLIENTEDGE, kDropLabelId);
    ownedState->fileList = createChild(window, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL, WS_EX_CLIENTEDGE, kFileListId);
    ownedState->typeList = createChild(window, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL, WS_EX_CLIENTEDGE, kTypeListId);
    ownedState->statusLabel = createChild(window, L"STATIC", L"Drop or select a CAD file to preview point types.", SS_LEFT | SS_CENTERIMAGE, 0, kStatusLabelId);

    for (HWND handle : {ownedState->pickButton, ownedState->exportButton, ownedState->dropLabel, ownedState->fileList, ownedState->typeList, ownedState->statusLabel}) {
      SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(ownedState->uiFont), TRUE);
    }

    ListView_SetExtendedListViewStyle(ownedState->fileList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    ListView_SetExtendedListViewStyle(ownedState->typeList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
    addColumn(ownedState->fileList, 0, L"File", 300);
    addColumn(ownedState->fileList, 1, L"Status", 85);
    addColumn(ownedState->fileList, 2, L"Result", 150);
    addColumn(ownedState->typeList, 0, L"Point type", 300);
    addColumn(ownedState->typeList, 1, L"Count", 80);
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

  case WM_COMMAND:
    if (LOWORD(wParam) == kPickButtonId) {
      enqueueFiles(window, pickFiles(window));
    } else if (LOWORD(wParam) == kExportButtonId) {
      startExport(window);
    }
    return 0;

  case WM_NOTIFY:
    if (state && reinterpret_cast<NMHDR*>(lParam)->idFrom == kFileListId &&
        reinterpret_cast<NMHDR*>(lParam)->code == LVN_ITEMCHANGED) {
      populateTypeList(*state, selectedFileIndex(state->fileList));
    }
    return 0;

  case WM_DROPFILES: {
    const auto drop = reinterpret_cast<HDROP>(wParam);
    enqueueFiles(window, pathsFromDrop(drop));
    DragFinish(drop);
    return 0;
  }

  case kPreviewDoneMessage: {
    std::unique_ptr<FinishedPreview> finished(reinterpret_cast<FinishedPreview*>(lParam));
    if (state && finished && finished->row >= 0 && finished->row < static_cast<int>(state->files.size())) {
      auto& preview = state->files[static_cast<std::size_t>(finished->row)];
      preview.path = std::move(finished->path);
      preview.readResult = std::move(finished->result);
      preview.loaded = true;

      if (preview.readResult.hasErrors()) {
        updateFileRow(state->fileList, finished->row, L"Failed", L"See log");
      } else {
        updateFileRow(state->fileList, finished->row, L"Ready", std::to_wstring(preview.readResult.points.size()) + L" points");
      }

      if (selectedFileIndex(state->fileList) == finished->row) {
        populateTypeList(*state, finished->row);
      }
    }
    return 0;
  }

  case kExportDoneMessage: {
    std::unique_ptr<FinishedExport> finished(reinterpret_cast<FinishedExport*>(lParam));
    if (state && finished) {
      const int index = selectedFileIndex(state->fileList);
      const auto succeeded = finished->result.status == dgm2xyz::ConversionStatus::Succeeded;
      if (index >= 0) {
        updateFileRow(state->fileList, index, succeeded ? L"Exported" : L"Failed", widenUtf8(finished->result.summary()));
      }
      setWindowText(state->statusLabel, widenUtf8(finished->result.summary()));
      EnableWindow(state->exportButton, TRUE);
    }
    return 0;
  }

  case WM_DESTROY:
    DragAcceptFiles(window, FALSE);
    delete state;
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    PostQuitMessage(0);
    return 0;
  }

  return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
  initializeLogging();

  INITCOMMONCONTROLSEX controls{};
  controls.dwSize = sizeof(controls);
  controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&controls);

  const wchar_t* className = L"dgm2xyz.MainWindow";

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
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT,
                                CW_USEDEFAULT,
                                920,
                                560,
                                nullptr,
                                nullptr,
                                instance,
                                nullptr);

  if (!window) {
    return 1;
  }

  ShowWindow(window, showCommand);
  UpdateWindow(window);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  return static_cast<int>(message.wParam);
}
