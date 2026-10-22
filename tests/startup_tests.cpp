#include "Platform.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Child {
  PROCESS_INFORMATION process{};
  explicit Child(const std::filesystem::path &exe) {
    STARTUPINFOW startup{sizeof(startup)};
    auto command = app::quote_arg(exe.wstring()) + L" --startup";
    require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, exe.parent_path().c_str(),
                           &startup, &process), "Could not launch startup test");
    CloseHandle(process.hThread);
  }
  ~Child() {
    // Only terminate the process created by this test, never a user's tray app.
    TerminateProcess(process.hProcess, 0);
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hProcess);
  }
};
struct Windows {
  DWORD pid;
  bool visible = false;
};
BOOL CALLBACK inspect_window(HWND window, LPARAM data) {
  auto &state = *reinterpret_cast<Windows *>(data);
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  wchar_t type[256]{};
  GetClassNameW(window, type, 256);
  // Winit's transparent event target uses WS_VISIBLE to receive WM_PAINT;
  // it is an internal message window, not an app popup.
  if (pid == state.pid && IsWindowVisible(window) &&
      std::wstring(type) != L"Winit Thread Event Target") {
    state.visible = true;
  }
  return TRUE;
}
bool visible(DWORD pid) {
  Windows state{pid};
  EnumWindows(inspect_window, reinterpret_cast<LPARAM>(&state));
  return state.visible;
}
void require_hidden(const Child &child, DWORD duration) {
  auto deadline = GetTickCount64() + duration;
  do {
    require(WaitForSingleObject(child.process.hProcess, 0) == WAIT_TIMEOUT,
            "Startup process exited unexpectedly");
    require(!visible(child.process.dwProcessId), "Startup opened a window");
    Sleep(25);
  } while (GetTickCount64() < deadline);
}
void startup_case(const std::filesystem::path &exe) {
  Child child(exe);
  bool ready = false;
  for (int attempt = 0; attempt < 200 && !ready; ++attempt) {
    require_hidden(child, 25);
    DWORD pid = 0;
    GetWindowThreadProcessId(FindWindowW(L"ScrcpyTrayMsgWnd", nullptr), &pid);
    ready = pid == child.process.dwProcessId;
  }
  require(ready, "Startup did not create its tray window");
  require_hidden(child, 2000);
  auto duplicate = app::run_process(exe.wstring(), {L"--startup"}, {}, 3000);
  require(duplicate.code == 0 && !duplicate.timed_out,
          "Duplicate startup did not exit successfully");
  require_hidden(child, 1000);

  // Manual activation must still work after a quiet sign-in launch.
  auto manual = app::run_process(exe.wstring(), {}, {}, 3000);
  require(manual.code == 0 && !manual.timed_out, "Manual activation failed");
  bool shown = false;
  for (int attempt = 0; attempt < 80 && !shown; ++attempt) {
    shown = visible(child.process.dwProcessId);
    Sleep(25);
  }
  require(shown, "Manual activation did not open the running GUI");
}
} // namespace

int main(int argc, char **argv) {
  if (FindWindowW(L"ScrcpyTrayMsgWnd", nullptr)) {
    std::cout << "Quit the running tray app to test automatic startup.\n";
    return 77;
  }
  try {
    require(argc == 4, "Expected GUI, fixture and work directory");
    const auto source = std::filesystem::absolute(app::wide(argv[1]));
    const auto fixture = std::filesystem::absolute(app::wide(argv[2]));
    auto root = std::filesystem::absolute(app::wide(argv[3])) /
                ("case with spaces " + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    auto exe = root / "scrcpy-ui.exe";
    std::filesystem::copy_file(source, exe);
    for (const auto &entry : std::filesystem::directory_iterator(source.parent_path()))
      if (entry.path().extension() == L".dll")
        std::filesystem::copy_file(entry.path(), root / entry.path().filename());

    // Child-only test environment: no saved configuration, real phone tools,
    // updater script/network traffic, or changes to the user's startup entry.
    SetEnvironmentVariableW(L"APPDATA", (root / "settings").c_str());
    SetEnvironmentVariableW(L"LOCALAPPDATA", (root / "local").c_str());
    SetEnvironmentVariableW(L"SCRCPY", nullptr);
    SetEnvironmentVariableW(L"PATH", L"");
    require(SetCurrentDirectoryW(root.c_str()), "Could not isolate test directory");
    startup_case(exe);
    std::cout << "PASS: unconfigured startup stays hidden and activates manually\n";

    std::filesystem::copy_file(fixture, root / "scrcpy.exe");
    std::filesystem::copy_file(fixture, root / "adb.exe");
    SetEnvironmentVariableW(L"SCRCPY", (root / "scrcpy.exe").c_str());
    startup_case(exe);
    std::cout << "PASS: configured startup and duplicate stay hidden without mirroring\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
