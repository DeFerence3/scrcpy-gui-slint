#include "AppCore.h"
#include "Platform.h"
#include <shellapi.h>
#include <fstream>
#include <iostream>
#include <thread>

void require(bool test, const char *message) {
  if (!test) throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  // A harmless child process used to verify real CreateProcess quoting and cancellation.
  if (argc > 1 && std::string(argv[1]) == "--echo") {
    for (int i=2; i<argc; ++i) std::cout << argv[i] << '\n';
    return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--wait") { Sleep(10000); return 0; }
  try {
    auto devices = app::parse_devices("* daemon started successfully *\r\nList of devices attached\r\nABC device product:foo model:Pixel_7_Pro transport_id:1\r\n192.168.1.4:5555 device model:Galaxy_S23\r\nLOCK unauthorized\r\nGONE offline\r\nABC device\r\n");
    require(devices.size() == 4, "ADB states or deduplication failed");
    require(devices[0].name == "Pixel 7 Pro" && devices[0].ready(), "ADB model parsing failed");
    require(devices[1].connection == "Wireless" && !devices[2].ready() && !devices[3].ready(), "Device eligibility failed");
    require(app::parse_devices("error: device not found\n").empty(), "Error parsed as device");
    require(app::valid_repository("DeFerence3/scrcpy-pad-slint") && !app::valid_repository("../bad") && !app::valid_repository("https://github.com/o/r"), "Repository validation failed");
    UINT modifiers, key; std::string canonical;
    require(app::parse_hotkey("shift + Ctrl + f12", modifiers, key, canonical) && key == VK_F12 && canonical == "Ctrl + Shift + F12", "Shortcut parsing failed");
    for (auto invalid : {"S", "Ctrl +", "Ctrl + S + T", "Ctrl + Ctrl + S", "Alt + F25", "Win + F999999999"})
      require(!app::parse_hotkey(invalid, modifiers, key, canonical), "Invalid shortcut accepted");
    std::string error;
    auto args = app::template_args("--record=\"C:\\My Videos\\demo.mp4\" --no-audio", error);
    require(error.empty() && args.size() == 2 && args[0] == L"--record=C:\\My Videos\\demo.mp4", "Template quoting failed");
    for (auto invalid : {"--serial=other", "-sother", "--select-usb", "--tcpip=host", "--record=\"missing"}) {
      error.clear(); app::template_args(invalid, error); require(!error.empty(), "Unsafe device override accepted");
    }
    std::vector<std::wstring> values{L"", L"simple", L"C:\\path with spaces\\", L"has\"quote", L"& echo should-not-run"};
    std::wstring line = L"test";
    for (const auto &v : values) line += L" " + app::quote_arg(v);
    int count; auto parsed = CommandLineToArgvW(line.c_str(), &count);
    require(count == (int)values.size()+1, "Quoted argument count wrong");
    for (int i=1; i<count; ++i) require(parsed[i] == values[i-1], "Quoted argument round trip failed");
    LocalFree(parsed);
    auto directory = std::filesystem::temp_directory_path() / ("scrcpy-pad-test-" + std::to_string(GetCurrentProcessId()));
    auto path = directory / "settings.dat";
    app::Settings settings; settings.scrcpy_path = "C:\\测试 path\\scrcpy.exe";
    settings.templates.push_back({"My \"quoted\" template", "--record=\"C:\\videos\\record.mp4\""});
    settings.default_template = 3; settings.auto_update = true;
    error.clear(); require(app::save_settings(path, settings, error), "Settings save failed");
    app::Settings loaded; require(app::load_settings(path, loaded, error), "Settings load failed");
    require(loaded.scrcpy_path == settings.scrcpy_path && loaded.templates[3].name == settings.templates[3].name && loaded.default_template == 3 && loaded.auto_update, "Settings round trip failed");
    std::ofstream(path, std::ios::trunc) << "broken";
    require(!app::load_settings(path, loaded, error) && loaded.templates.size() == 4, "Corrupt settings changed current state");
    std::filesystem::remove(path); std::filesystem::remove(directory);
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, 32768);
    auto result = app::run_process(exe, {L"--echo", L"space arg", L"\"quoted\"", L"C:\\ending\\", L"& echo literal"});
    require(result.code == 0 && result.text == "space arg\r\n\"quoted\"\r\nC:\\ending\\\r\n& echo literal\r\n", "Process argument round trip failed");
    result = app::run_process(exe, {L"--wait"}, {}, 100);
    require(result.timed_out && result.code != 0, "Process timeout failed");
    auto cancel = std::make_shared<app::Cancel>();
    std::thread stopper([cancel] { Sleep(100); cancel->requested = true; });
    result = app::run_process(exe, {L"--wait"}, {}, 0, cancel);
    stopper.join(); require(!result.timed_out && result.code != 0, "Process cancellation failed");
    result = app::run_process(L"Z:\\nonexistent\\scrcpy.exe", {});
    require(result.code != 0 && result.text.find("Could not start") != std::string::npos, "Launch failure was not reported");
    std::cout << "All core checks passed.\n";
    return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
