#include "Scrcpy.h"
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <thread>

namespace {
bool is_file(const std::wstring &path) {
  DWORD attrs = GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring find_on_path(const wchar_t *name) {
  wchar_t buffer[32768]{};
  DWORD count = SearchPathW(nullptr, name, L".exe", 32768, buffer, nullptr);
  return count && count < 32768 ? buffer : L"";
}
} // namespace
Scrcpy::Scrcpy(Output output, Session session)
    : output_(std::move(output)), session_(std::move(session)) {}
Scrcpy::~Scrcpy() { stop(); }
bool Scrcpy::set_path(std::wstring path) {
  auto extension = std::filesystem::path(path).extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t c) { return (wchar_t)std::towlower(c); });
  if (!is_file(path) || extension != L".exe") {
    output_("Select an existing scrcpy executable (.exe).\n");
    return false;
  }
  path_ = std::filesystem::absolute(path).wstring();
  output_("[scrcpy] " + app::utf8(path_) + "\n");
  return true;
}
bool Scrcpy::detect(const std::string &saved) {
  if (!saved.empty() && is_file(app::wide(saved)))
    return set_path(app::wide(saved));
  wchar_t env[32768]{};
  if (auto size = GetEnvironmentVariableW(L"SCRCPY", env, 32768);
      size && size < 32768 && is_file(env))
    return set_path(env);
  auto found = find_on_path(L"scrcpy");
  if (!found.empty())
    return set_path(found);
  std::ifstream legacy(app::config_directory() / "scrcpy_path.txt",
                       std::ios::binary);
  std::string old((std::istreambuf_iterator<char>(legacy)),
                  std::istreambuf_iterator<char>());
  if (!old.empty() && is_file(app::wide(old)))
    return set_path(app::wide(old));
  output_("[scrcpy] Not found. Choose scrcpy.exe in Settings.\n");
  return false;
}
std::wstring Scrcpy::adb_path() const {
  auto adjacent = std::filesystem::path(path_).parent_path() / "adb.exe";
  if (!path_.empty() && is_file(adjacent.wstring()))
    return adjacent.wstring();
  auto found = find_on_path(L"adb");
  return found.empty() ? L"adb.exe" : found;
}
void Scrcpy::version(std::function<void(std::string)> done) const {
  auto path = path_;
  std::thread([path, done] {
    auto result = app::run_process(path, {L"--version"}, {}, 5000);
    auto line = app::trim(result.text.substr(0, result.text.find('\n')));
    done(result.code == 0 ? line : "scrcpy version unavailable");
  }).detach();
}
void Scrcpy::list_devices(Devices done) const {
  auto adb = adb_path();
  std::thread([adb, done] {
    auto result = app::run_process(adb, {L"devices", L"-l"});
    done(result.code == 0 ? app::parse_devices(result.text)
                          : std::vector<app::Device>{},
         result.code == 0 ? "" : app::trim(result.text));
  }).detach();
}
void Scrcpy::restart_adb(std::function<void()> done) const {
  auto adb = adb_path();
  auto output = output_;
  std::thread([adb, output, done] {
    output("[adb] Restarting server...\n");
    app::run_process(adb, {L"kill-server"}, output);
    app::run_process(adb, {L"start-server"}, output);
    done();
  }).detach();
}
bool Scrcpy::start(const std::string &serial, const std::string &arguments) {
  if (path_.empty() || serial.empty()) {
    output_("Choose scrcpy.exe and a ready device first.\n");
    return false;
  }
  std::string error;
  auto args = app::template_args(arguments, error);
  if (!error.empty()) {
    output_(error + "\n");
    return false;
  }
  args.insert(args.begin(), L"--serial=" + app::wide(serial));
  auto cancel = std::make_shared<app::Cancel>();
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->sessions.count(serial)) {
      output_("This device already has a mirror session.\n");
      return false;
    }
    state_->sessions[serial] = cancel;
  }
  session_(serial, true);
  auto state = state_;
  auto path = path_;
  auto output = output_;
  auto session = session_;
  output("[mirror] Starting " + serial + " " + arguments + "\n");
  std::thread([state, path, args, output, session, serial, cancel] {
    auto result =
        app::run_process(path, args, output, 0, cancel,
                         std::filesystem::path(path).parent_path().wstring());
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->sessions.erase(serial);
    }
    output("[mirror] " + serial + " stopped (exit " +
           std::to_string(result.code) + ").\n");
    session(serial, false);
  }).detach();
  return true;
}
void Scrcpy::stop(const std::string &serial) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  for (const auto &[id, cancel] : state_->sessions)
    if (serial.empty() || serial == id)
      cancel->requested = true;
}
