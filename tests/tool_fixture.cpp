#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fstream>
#include <iostream>
#include <string>
int main(int argc, char **argv) {
  std::string first = argc > 1 ? argv[1] : "";
  if (first == "devices") {
    std::cout << "List of devices attached\nUSB123 device model:Pixel_7_Pro\n192.168.1.15:5555 device model:Galaxy_S23\nLOCK unauthorized\n";
  } else if (first == "--version") std::cout << "scrcpy fixture\n";
  else if (first == "--updated") std::ofstream("restarted.txt") << "updated";
  else if (first == "--startup") std::ofstream("started.txt") << "--startup";
  else if (first == "--tray") {
    WNDCLASSW window{};
    window.lpfnWndProc = DefWindowProcW;
    window.hInstance = GetModuleHandleW(nullptr);
    window.lpszClassName = L"ScrcpyTrayMsgWnd";
    RegisterClassW(&window);
    auto tray = CreateWindowW(window.lpszClassName, L"", 0, 0, 0, 0, 0,
                              nullptr, nullptr, window.hInstance, nullptr);
    if (!tray) return 1;
    Sleep(30000);
    DestroyWindow(tray);
  }
  else if (first.rfind("--serial=", 0) == 0) {
    for (int i=1; i<argc; ++i) std::cout << argv[i] << '\n';
    std::cout.flush();
    Sleep(10000);
  }
  return 0;
}
