#include "dgm2xyz/Conversion.h"
#include "dgm2xyz/DxfPointReader.h"

#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>

#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr int kDropLabelId = 1001;
constexpr int kPickButtonId = 1002;
constexpr int kListViewId = 1003;
constexpr UINT kConversionDoneMessage = WM_APP + 1;

struct AppState {
  HWND dropLabel = nullptr;
  HWND pickButton = nullptr;
  HWND listView = nullptr;
  HFONT uiFont = nullptr;
};

struct FinishedConversion {
  int row = -1;
  dgm2xyz::ConversionResult result;
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

std::wstring statusText(const dgm2xyz::ConversionResult& result) {
  return widenUtf8(result.summary());
}

void setListText(HWND listView, int row, int column, const std::wstring& text) {
  LVITEMW item{};
  item.mask = LVIF_TEXT;
  item.iItem = row;
  item.iSubItem = column;
  item.pszText = const_cast<wchar_t*>(text.c_str());
  ListView_SetItem(listView, &item);
}

int addFileRow(HWND listView, const std::filesystem::path& path) {
  const auto pathText = path.wstring();

  LVITEMW item{};
  item.mask = LVIF_TEXT;
  item.iItem = ListView_GetItemCount(listView);
  item.pszText = const_cast<wchar_t*>(pathText.c_str());
  const int row = ListView_InsertItem(listView, &item);

  setListText(listView, row, 1, L"Queued");
  setListText(listView, row, 2, L"");
  return row;
}

void runConversion(HWND window, int row, std::filesystem::path path) {
  setListText(GetDlgItem(window, kListViewId), row, 1, L"Running");

  std::thread([window, row, path = std::move(path)] {
    auto finished = std::make_unique<FinishedConversion>();
    finished->row = row;
    finished->result = dgm2xyz::convertFile(path, dgm2xyz::DxfPointReader{});
    PostMessageW(window, kConversionDoneMessage, 0, reinterpret_cast<LPARAM>(finished.release()));
  }).detach();
}

void enqueueFiles(HWND window, const std::vector<std::filesystem::path>& paths) {
  const auto state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state || !state->listView) {
    return;
  }

  for (const auto& path : paths) {
    const int row = addFileRow(state->listView, path);
    runConversion(window, row, path);
  }
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
  dialog.lpstrFilter = L"CAD files (*.dxf;*.dwg)\0*.dxf;*.dwg\0DXF files (*.dxf)\0*.dxf\0All files (*.*)\0*.*\0";
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

void addColumn(HWND listView, int index, const wchar_t* text, int width) {
  LVCOLUMNW column{};
  column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  column.pszText = const_cast<wchar_t*>(text);
  column.cx = width;
  column.iSubItem = index;
  ListView_InsertColumn(listView, index, &column);
}

void layoutControls(HWND window, AppState& state) {
  RECT rect{};
  GetClientRect(window, &rect);
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  const int margin = 12;
  const int buttonHeight = 30;
  const int dropHeight = 82;

  MoveWindow(state.pickButton, margin, margin, 150, buttonHeight, TRUE);
  MoveWindow(state.dropLabel, margin, margin + buttonHeight + 10, width - margin * 2, dropHeight, TRUE);
  MoveWindow(state.listView,
             margin,
             margin + buttonHeight + 10 + dropHeight + 10,
             width - margin * 2,
             height - (margin + buttonHeight + 10 + dropHeight + 10) - margin,
             TRUE);

  ListView_SetColumnWidth(state.listView, 0, width - 340);
  ListView_SetColumnWidth(state.listView, 1, 100);
  ListView_SetColumnWidth(state.listView, 2, 200);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  auto state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

  switch (message) {
  case WM_CREATE: {
    auto ownedState = std::make_unique<AppState>();
    ownedState->uiFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    ownedState->pickButton = CreateWindowExW(0,
                                             L"BUTTON",
                                             L"Select files...",
                                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                             0,
                                             0,
                                             0,
                                             0,
                                             window,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPickButtonId)),
                                             nullptr,
                                             nullptr);

    ownedState->dropLabel = CreateWindowExW(WS_EX_CLIENTEDGE,
                                            L"STATIC",
                                            L"Drop DXF files here",
                                            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                            0,
                                            0,
                                            0,
                                            0,
                                            window,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDropLabelId)),
                                            nullptr,
                                            nullptr);

    ownedState->listView = CreateWindowExW(WS_EX_CLIENTEDGE,
                                           WC_LISTVIEWW,
                                           L"",
                                           WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                           0,
                                           0,
                                           0,
                                           0,
                                           window,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListViewId)),
                                           nullptr,
                                           nullptr);

    SendMessageW(ownedState->pickButton, WM_SETFONT, reinterpret_cast<WPARAM>(ownedState->uiFont), TRUE);
    SendMessageW(ownedState->dropLabel, WM_SETFONT, reinterpret_cast<WPARAM>(ownedState->uiFont), TRUE);
    SendMessageW(ownedState->listView, WM_SETFONT, reinterpret_cast<WPARAM>(ownedState->uiFont), TRUE);

    ListView_SetExtendedListViewStyle(ownedState->listView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    addColumn(ownedState->listView, 0, L"File", 420);
    addColumn(ownedState->listView, 1, L"Status", 100);
    addColumn(ownedState->listView, 2, L"Result", 220);

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
    }
    return 0;

  case WM_DROPFILES: {
    const auto drop = reinterpret_cast<HDROP>(wParam);
    enqueueFiles(window, pathsFromDrop(drop));
    DragFinish(drop);
    return 0;
  }

  case kConversionDoneMessage: {
    std::unique_ptr<FinishedConversion> finished(reinterpret_cast<FinishedConversion*>(lParam));
    if (state && finished) {
      const auto succeeded = finished->result.status == dgm2xyz::ConversionStatus::Succeeded;
      setListText(state->listView, finished->row, 1, succeeded ? L"Done" : L"Failed");
      setListText(state->listView, finished->row, 2, statusText(finished->result));
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
                                760,
                                460,
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
