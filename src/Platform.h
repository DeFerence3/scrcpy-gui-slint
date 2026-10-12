#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace app {
std::wstring wide(const std::string &value);
std::string utf8(const std::wstring &value);
std::wstring quote_arg(const std::wstring &value);
std::vector<std::wstring> template_args(const std::string &text,
                                        std::string &error);
std::filesystem::path config_directory();
std::filesystem::path executable_directory();
std::string system_error(DWORD code = GetLastError());
bool parse_hotkey(const std::string &text, UINT &modifiers, UINT &key,
                  std::string &canonical);
struct Cancel {
  std::atomic<bool> requested{false};
};
struct ProcessResult {
  DWORD code = 1;
  std::string text;
  bool timed_out = false;
};
using Output = std::function<void(std::string)>;
ProcessResult run_process(const std::wstring &executable,
                          const std::vector<std::wstring> &args,
                          Output output = {}, DWORD timeout_ms = 30000,
                          std::shared_ptr<Cancel> cancel = {},
                          const std::wstring &directory = {});
bool launch_detached(const std::wstring &executable,
                     const std::vector<std::wstring> &args, std::string &error);
} // namespace app
