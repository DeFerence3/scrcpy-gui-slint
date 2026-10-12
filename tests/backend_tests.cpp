#include "Scrcpy.h"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <set>

int main(int argc, char **argv) {
  try {
    if (argc < 2) throw std::runtime_error("Missing fixture");
    auto dir = std::filesystem::current_path() / "backend-fixtures" / std::to_string(GetCurrentProcessId());
    std::filesystem::create_directories(dir / "space & path");
    auto executable = dir / "space & path/scrcpy.exe";
    std::filesystem::copy_file(argv[1], executable);
    std::filesystem::copy_file(argv[1], executable.parent_path() / "adb.exe");
    std::mutex mutex; std::condition_variable cv;
    std::string log, failure; std::vector<app::Device> devices;
    std::set<std::string> active; bool listed = false; int finished = 0;
    Scrcpy backend([&](std::string value) { std::lock_guard<std::mutex> lock(mutex); log += value; cv.notify_all(); },
      [&](std::string serial, bool running) {
        std::lock_guard<std::mutex> lock(mutex);
        if (running) active.insert(serial); else { active.erase(serial); ++finished; }
        cv.notify_all();
      });
    auto require = [](bool okay, const char *message) { if (!okay) throw std::runtime_error(message); };
    require(backend.set_path(executable.wstring()), "Could not set fixture path");
    backend.list_devices([&](std::vector<app::Device> value, std::string error) {
      std::lock_guard<std::mutex> lock(mutex); devices = value; failure = error; listed = true; cv.notify_all();
    });
    {
      std::unique_lock<std::mutex> lock(mutex);
      require(cv.wait_for(lock, std::chrono::seconds(5), [&] { return listed; }), "Device discovery timed out");
      require(failure.empty() && devices.size() == 3 && devices[0].name == "Pixel 7 Pro", "Real fixture discovery failed");
    }
    require(backend.start("USB123", "--record=\"C:\\My Videos\\record.mp4\""), "First mirror did not start");
    require(backend.start("192.168.1.15:5555", "--no-audio"), "Second mirror did not start");
    require(!backend.start("USB123", ""), "Duplicate mirror was accepted");
    {
      std::unique_lock<std::mutex> lock(mutex);
      require(cv.wait_for(lock, std::chrono::seconds(5), [&] { return log.find("--record=C:\\My Videos\\record.mp4") != std::string::npos && log.find("--serial=192.168.1.15:5555") != std::string::npos; }), "Arguments did not reach mirror processes");
    }
    backend.stop("USB123");
    {
      std::unique_lock<std::mutex> lock(mutex);
      require(cv.wait_for(lock, std::chrono::seconds(5), [&] { return finished == 1; }), "Targeted stop timed out");
      require(active.size() == 1 && active.count("192.168.1.15:5555"), "Targeted stop affected another session");
    }
    backend.stop();
    {
      std::unique_lock<std::mutex> lock(mutex);
      require(cv.wait_for(lock, std::chrono::seconds(5), [&] { return finished == 2; }), "Stop-all timed out");
    }
    std::cout << "Device discovery, argument forwarding, duplicate prevention and isolated stop passed.\n";
    return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
