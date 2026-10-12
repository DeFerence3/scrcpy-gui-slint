#include "Platform.h"
#include "AppCore.h"
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace app {
namespace {
struct Handle {
  HANDLE value = nullptr;
  explicit Handle(HANDLE h = nullptr): value(h) {}
  ~Handle() { close(); }
  void close() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = nullptr; }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
};
std::wstring command_line(const std::wstring &exe, const std::vector<std::wstring> &args) {
  auto line = quote_arg(exe);
  for (const auto &arg : args) line += L" " + quote_arg(arg);
  return line;
}
}
std::wstring wide(const std::string &value) {
  int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0);
  std::wstring result(count, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), count);
  return result;
}
std::string utf8(const std::wstring &value) {
  int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0, nullptr, nullptr);
  std::string result(count, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), count, nullptr, nullptr);
  return result;
}
std::string system_error(DWORD code) {
  wchar_t buffer[1024]{};
  FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buffer, 1024, nullptr);
  return trim(utf8(buffer)) + " (" + std::to_string(code) + ")";
}
std::wstring quote_arg(const std::wstring &value) {
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (auto c : value) {
    if (c == L'\\') { ++backslashes; continue; }
    out.append(backslashes * (c == L'"' ? 2 : 1), L'\\');
    backslashes = 0;
    if (c == L'"') out += L'\\';
    out += c;
  }
  out.append(backslashes * 2, L'\\');
  return out + L'"';
}
std::vector<std::wstring> template_args(const std::string &text, std::string &error) {
  if (text.size() > 4096 || text.find_first_of("\r\n") != std::string::npos) {
    error = "Arguments must be a single line of at most 4096 characters."; return {};
  }
  // Reject unmatched quotes before using Windows' standard argv parser.
  bool quoted = false;
  size_t slashes = 0;
  for (char c : text) {
    if (c == '"' && slashes % 2 == 0) quoted = !quoted;
    slashes = c == '\\' ? slashes + 1 : 0;
  }
  if (quoted) { error = "Close the quoted argument before saving."; return {}; }
  int count = 0;
  auto argv = CommandLineToArgvW((L"scrcpy " + wide(text)).c_str(), &count);
  if (!argv) { error = system_error(); return {}; }
  std::vector<std::wstring> result;
  for (int i=1; i<count; ++i) {
    std::wstring arg = argv[i];
    auto name = arg.substr(0, arg.find(L'='));
    if (name == L"--serial" || name == L"--select-usb" || name == L"--select-tcpip" || name == L"--tcpip"
        || name == L"-d" || name == L"-e" || arg.rfind(L"-s", 0) == 0) {
      error = "Choose devices in the Devices tab; remove device-selection arguments.";
      break;
    }
    result.push_back(std::move(arg));
  }
  LocalFree(argv);
  return error.empty() ? result : std::vector<std::wstring>{};
}
std::filesystem::path config_directory() {
  wchar_t dir[32768]{};
  DWORD size = GetEnvironmentVariableW(L"APPDATA", dir, 32768);
  if (!size || size >= 32768) throw std::runtime_error("APPDATA is unavailable.");
  return std::filesystem::path(dir) / "scrcpy-tray";
}
std::filesystem::path executable_directory() {
  wchar_t path[32768]{};
  GetModuleFileNameW(nullptr, path, 32768);
  return std::filesystem::path(path).parent_path();
}
bool parse_hotkey(const std::string &text, UINT &modifiers, UINT &key, std::string &canonical) {
  modifiers = 0; key = 0; canonical.clear();
  std::istringstream parts(text);
  std::string part;
  while (std::getline(parts, part, '+')) {
    part = trim(part);
    std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    UINT modifier = part == "CTRL" || part == "CONTROL" ? MOD_CONTROL : part == "ALT" ? MOD_ALT
      : part == "SHIFT" ? MOD_SHIFT : part == "WIN" ? MOD_WIN : 0;
    if (modifier) { if (modifiers & modifier) return false; modifiers |= modifier; continue; }
    if (key) return false;
    if (part.size() == 1 && ((part[0] >= 'A' && part[0] <= 'Z') || (part[0] >= '0' && part[0] <= '9'))) key = part[0];
    else if (part.size() >= 2 && part[0] == 'F' && part.substr(1).find_first_not_of("0123456789") == std::string::npos) {
      if (part.size() > 3) return false;
      int f = std::stoi(part.substr(1));
      if (f < 1 || f > 24) return false;
      key = VK_F1 + f - 1;
    } else return false;
  }
  if (!key || !modifiers || (!text.empty() && text.back() == '+')) return false;
  if (modifiers & MOD_CONTROL) canonical += "Ctrl + ";
  if (modifiers & MOD_ALT) canonical += "Alt + ";
  if (modifiers & MOD_SHIFT) canonical += "Shift + ";
  if (modifiers & MOD_WIN) canonical += "Win + ";
  canonical += key >= VK_F1 && key <= VK_F24 ? "F" + std::to_string(key - VK_F1 + 1) : std::string(1, (char)key);
  return true;
}
ProcessResult run_process(const std::wstring &exe, const std::vector<std::wstring> &args,
                         Output output, DWORD timeout_ms, std::shared_ptr<Cancel> cancel, const std::wstring &directory) {
  ProcessResult result;
  auto fail = [&](const std::string &message) { result.text += message + "\n"; if (output) output(message + "\n"); };
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE read = nullptr, write = nullptr;
  if (!CreatePipe(&read, &write, &security, 0)) { fail(system_error()); return result; }
  Handle reader(read), writer(write);
  if (!SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0)) { fail(system_error()); return result; }
  Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
  if (input.value == INVALID_HANDLE_VALUE) { fail(system_error()); return result; }
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  std::vector<unsigned char> storage(bytes);
  auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) { fail(system_error()); return result; }
  HANDLE inherited[] = {writer.value, input.value};
  if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
    fail(system_error()); DeleteProcThreadAttributeList(attributes); return result;
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = writer.value;
  startup.StartupInfo.hStdInput = input.value;
  startup.lpAttributeList = attributes;
  Handle job(CreateJobObjectW(nullptr, nullptr));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  // adb may start its shared daemon. Keep that child outside our job so stopping
  // a mirror (or completing `adb devices`) cannot kill other apps' ADB sessions.
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
  if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    fail(system_error()); DeleteProcThreadAttributeList(attributes); return result;
  }
  PROCESS_INFORMATION info{};
  auto line = command_line(exe, args);
  // CreateProcess (never cmd.exe): template arguments cannot execute shell commands.
  BOOL started = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE,
    CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
    directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &info);
  DWORD launch_error = GetLastError();
  DeleteProcThreadAttributeList(attributes);
  if (!started) { fail("Could not start " + utf8(exe) + ": " + system_error(launch_error)); return result; }
  Handle process(info.hProcess), thread(info.hThread);
  if (!AssignProcessToJobObject(job.value, process.value)) {
    fail(system_error()); TerminateProcess(process.value, 1); return result;
  }
  if (ResumeThread(thread.value) == (DWORD)-1) { fail(system_error()); TerminateProcess(process.value, 1); return result; }
  writer.close(); input.close();
  auto began = GetTickCount64();
  std::string pending;
  bool terminated = false;
  for (;;) {
    if (!terminated && ((cancel && cancel->requested) || (timeout_ms && GetTickCount64() - began > timeout_ms))) {
      result.timed_out = !(cancel && cancel->requested);
      TerminateJobObject(job.value, ERROR_CANCELLED);
      terminated = true;
      if (result.timed_out) fail("Command timed out.");
    }
    DWORD available = 0;
    if (PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr) && available) {
      char buffer[4096]; DWORD count = 0;
      if (ReadFile(reader.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr) && count) {
        if (result.text.size() < 1024*1024) result.text.append(buffer, count);
        pending.append(buffer, count);
        auto end = pending.find_last_of('\n');
        if (end != std::string::npos) { if (output) output(pending.substr(0, end+1)); pending.erase(0, end+1); }
        // Also bound output from tools that never write a newline. Keep the last
        // few bytes together so a UTF-8 character is not split across callbacks.
        if (pending.size() > 8192) {
          size_t end = pending.size() - 4;
          while (end && (static_cast<unsigned char>(pending[end]) & 0xc0) == 0x80) --end;
          if (output) output(pending.substr(0, end));
          pending.erase(0, end);
        }
        continue;
      }
    }
    if (WaitForSingleObject(process.value, 20) == WAIT_OBJECT_0) {
      if (!PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr) || !available) break;
    }
  }
  if (output && !pending.empty()) output(pending + "\n");
  GetExitCodeProcess(process.value, &result.code);
  return result;
}
bool launch_detached(const std::wstring &exe, const std::vector<std::wstring> &args, std::string &error) {
  auto line = command_line(exe, args);
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
    error = system_error(); return false;
  }
  CloseHandle(info.hProcess); CloseHandle(info.hThread); return true;
}
}
