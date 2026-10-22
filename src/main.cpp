#include "main.h"
#include "Scrcpy.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <shellapi.h>
#include <shobjidl.h>
#include <sstream>
#include <thread>

#ifndef APP_VERSION
#define APP_VERSION "0.1.0"
#endif

namespace {
constexpr UINT TrayMessage = WM_APP + 1;
constexpr UINT ShowCommand = 1001, MirrorCommand = 1002, QuitCommand = 1003;
const wchar_t *WindowTitle = L"Scrcpy Tray Tool";
std::string text(const slint::SharedString &s) {
  return std::string(s.data(), s.size());
}
std::wstring powershell() {
  wchar_t system[32768]{};
  GetSystemDirectoryW(system, 32768);
  return std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
}
class Controller;
Controller *controller = nullptr;
LRESULT CALLBACK tray_proc(HWND, UINT, WPARAM, LPARAM);

class Controller : public std::enable_shared_from_this<Controller> {
public:
  slint::ComponentHandle<MainWindow> ui = MainWindow::create();
  app::Settings settings;
  std::unique_ptr<Scrcpy> scrcpy;
  HWND tray = nullptr;
  NOTIFYICONDATAW icon{};
  bool visible = false, picking = false, focused = false, pending_quick = false;
  bool shutting_down = false, settings_writable = true, installing = false,
       auto_install_attempted = false;
  ULONGLONG shown_at = 0, hidden_at = 0;
  int hotkey_id = 0;
  std::vector<app::Device> devices;
  std::set<std::string> sessions;
  std::string available_version;
  std::filesystem::path settings_path;
  slint::Timer focus_timer, device_timer, update_timer;

  static void post(std::weak_ptr<Controller> weak,
                   std::function<void(Controller &)> callback) {
    slint::invoke_from_event_loop([weak, callback = std::move(callback)] {
      if (auto self = weak.lock(); self && !self->shutting_down)
        callback(*self);
    });
  }
  void log(std::string message) {
    auto value = text(ui->get_console_text()) + std::move(message);
    constexpr size_t limit = 48000;
    if (value.size() > limit) {
      auto end = value.find('\n', value.size() - limit);
      if (end != std::string::npos)
        value.erase(0, end + 1);
      else
        value = app::utf8(app::wide(value.substr(value.size() - limit)));
    }
    ui->set_console_text(slint::SharedString(value));
  }
  void status(const std::string &message) {
    ui->set_status_text(slint::SharedString(message));
  }
  bool save() {
    if (!settings_writable) {
      status("Settings could not be loaded. Back up or repair settings.dat "
             "before saving.");
      return false;
    }
    std::string error;
    if (!app::save_settings(settings_path, settings, error)) {
      log(error + "\n");
      status(error);
      return false;
    }
    return true;
  }
  void show(bool toggle = false) {
    if (toggle && GetTickCount64() - hidden_at < 400)
      return;
    if (visible && toggle) {
      hide();
      return;
    }
    ui->show();
    visible = true;
    focused = false;
    shown_at = GetTickCount64();
    if (HWND hwnd = FindWindowW(nullptr, WindowTitle)) {
      RECT area{}, rect{};
      POINT cursor{};
      GetCursorPos(&cursor);
      MONITORINFO monitor{sizeof(monitor)};
      if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST),
                          &monitor))
        area = monitor.rcWork;
      else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
      GetWindowRect(hwnd, &rect);
      SetWindowPos(
          hwnd, HWND_TOPMOST,
          std::max<LONG>(area.left, area.right - (rect.right - rect.left) - 12),
          std::max<LONG>(area.top, area.bottom - (rect.bottom - rect.top) - 12),
          0, 0, SWP_NOSIZE);
      ShowWindow(hwnd, SW_SHOW);
      SetForegroundWindow(hwnd);
    }
  }
  void hide() {
    ui->hide();
    visible = focused = false;
    hidden_at = GetTickCount64();
  }
  void quit() {
    shutting_down = true;
    focus_timer.stop();
    device_timer.stop();
    update_timer.stop();
    scrcpy->stop();
    Shell_NotifyIconW(NIM_DELETE, &icon);
    if (hotkey_id)
      UnregisterHotKey(tray, hotkey_id);
    slint::quit_event_loop();
  }
  void publish_devices() {
    std::vector<DeviceRow> rows;
    for (const auto &d : devices) {
      bool running = sessions.count(d.serial) != 0;
      std::string detail = d.connection + " · " + d.serial;
      if (!d.ready())
        detail += " · " + d.state;
      if (running)
        detail += " · mirroring";
      DeviceRow row;
      row.serial = slint::SharedString(d.serial);
      row.name = slint::SharedString(d.name);
      row.detail = slint::SharedString(detail);
      row.ready = d.ready();
      row.running = running;
      rows.push_back(row);
    }
    for (const auto &serial : sessions) {
      if (std::none_of(devices.begin(), devices.end(),
                       [&](const auto &d) { return d.serial == serial; })) {
        DeviceRow row;
        row.serial = slint::SharedString(serial);
        row.name = slint::SharedString(serial);
        row.detail = "Mirror session · device disconnected";
        row.ready = false;
        row.running = true;
        rows.push_back(row);
      }
    }
    ui->set_devices(std::make_shared<slint::VectorModel<DeviceRow>>(rows));
    ui->set_active_sessions((int)sessions.size());
  }
  void publish_templates() {
    std::vector<slint::SharedString> names;
    for (const auto &t : settings.templates)
      names.emplace_back(t.name);
    ui->set_template_names(
        std::make_shared<slint::VectorModel<slint::SharedString>>(names));
    ui->set_default_template(settings.default_template);
    if (ui->get_selected_template() < 0 ||
        ui->get_selected_template() >= (int)names.size())
      ui->set_selected_template(settings.default_template);
  }
  void edit_template(int index) {
    if (index < 0 || index >= (int)settings.templates.size())
      return;
    ui->set_editing_template(index);
    ui->set_template_name(slint::SharedString(settings.templates[index].name));
    ui->set_template_arguments(
        slint::SharedString(settings.templates[index].arguments));
  }
  void save_template() {
    auto name = app::trim(text(ui->get_template_name()));
    auto arguments = app::trim(text(ui->get_template_arguments()));
    if (name.empty() || name.size() > 80) {
      status("Enter a template name (1–80 characters).");
      return;
    }
    std::string error;
    app::template_args(arguments, error);
    if (!error.empty()) {
      status(error);
      return;
    }
    int index = ui->get_editing_template();
    for (int i = 0; i < (int)settings.templates.size(); ++i) {
      if (i != index && settings.templates[i].name == name) {
        status("A template with this name already exists.");
        return;
      }
    }
    auto previous = settings;
    if (index < 0) {
      if (settings.templates.size() >= 100) {
        status("You can save up to 100 templates.");
        return;
      }
      index = (int)settings.templates.size();
      settings.templates.push_back({name, arguments});
    } else if (index < (int)settings.templates.size())
      settings.templates[index] = {name, arguments};
    else
      return;
    if (!save()) {
      settings = previous;
      return;
    }
    publish_templates();
    ui->set_selected_template(index);
    edit_template(index);
    status("Template saved. Use Make default in Devices for Quick Mirror.");
  }
  void delete_template() {
    int index = ui->get_editing_template();
    if (index < 0 || index >= (int)settings.templates.size() ||
        settings.templates.size() <= 1)
      return;
    auto previous = settings;
    settings.templates.erase(settings.templates.begin() + index);
    if (settings.default_template == index)
      settings.default_template = 0;
    else if (settings.default_template > index)
      --settings.default_template;
    if (!save()) {
      settings = previous;
      return;
    }
    ui->set_selected_template(settings.default_template);
    publish_templates();
    edit_template(settings.default_template);
    status("Template deleted.");
  }
  void mirror(const std::string &serial, bool use_default = false) {
    if (installing) {
      status("Wait for the update installation to finish.");
      return;
    }
    if (scrcpy->path().empty()) {
      ui->set_page(1);
      show();
      status("Choose scrcpy.exe before mirroring.");
      return;
    }
    auto d =
        std::find_if(devices.begin(), devices.end(),
                     [&](const auto &item) { return item.serial == serial; });
    if (d == devices.end() || !d->ready()) {
      status("That device is not ready. Refresh and accept USB debugging on "
             "the phone.");
      return;
    }
    int index =
        use_default ? settings.default_template : ui->get_selected_template();
    if (index < 0 || index >= (int)settings.templates.size())
      return;
    if (scrcpy->start(serial, settings.templates[index].arguments))
      status("Starting " + d->name + " with " + settings.templates[index].name +
             ".");
  }
  void refresh(bool quick = false, bool silent = false) {
    pending_quick |= quick;
    if (ui->get_scanning())
      return;
    if (quick && scrcpy->path().empty()) {
      pending_quick = false;
      ui->set_page(1);
      show();
      status("Select scrcpy.exe in Settings to get started.");
      return;
    }
    ui->set_scanning(true);
    auto weak = weak_from_this();
    scrcpy->list_devices([weak, silent](std::vector<app::Device> list,
                                        std::string error) {
      post(weak, [list = std::move(list), error, silent](Controller &c) {
        c.ui->set_scanning(false);
        c.devices = list;
        c.publish_devices();
        bool quick = c.pending_quick;
        c.pending_quick = false;
        if (!error.empty()) {
          if (!silent || quick) {
            c.log("[adb] " + error + "\n");
            c.status("ADB unavailable. Check the scrcpy folder or PATH; "
                     "details are in Logs.");
          }
          if (quick)
            c.show();
          return;
        }
        if (!silent || quick)
          c.log("[adb] Found " + std::to_string(list.size()) + " device(s).\n");
        if (quick) {
          std::vector<app::Device> ready;
          for (const auto &d : list)
            if (d.ready())
              ready.push_back(d);
          if (ready.size() == 1)
            c.mirror(ready.front().serial, true);
          else {
            c.ui->set_page(0);
            c.ui->set_selected_template(c.settings.default_template);
            c.show();
            c.status(ready.empty() ? "No ready device. Connect a phone and "
                                     "accept USB debugging."
                                   : "Choose a device below to mirror with the "
                                     "default template.");
          }
        } else if (!silent)
          c.status(list.empty()
                       ? "Connect a phone and accept the USB debugging prompt."
                       : "Choose a device to mirror.");
      });
    });
  }
  void path_changed() {
    settings.scrcpy_path = app::utf8(scrcpy->path());
    ui->set_scrcpy_path(slint::SharedString(settings.scrcpy_path));
    save();
    auto weak = weak_from_this();
    auto expected = settings.scrcpy_path;
    scrcpy->version([weak, expected](std::string version) {
      post(weak, [version, expected](Controller &c) {
        if (c.settings.scrcpy_path == expected)
          c.ui->set_scrcpy_version(slint::SharedString(version));
      });
    });
    refresh();
  }
  void browse() {
    if (picking || installing)
      return;
    picking = true;
    HWND owner = FindWindowW(nullptr, WindowTitle);
    auto weak = weak_from_this();
    std::thread([weak, owner] {
      std::wstring selected;
      if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                COINIT_DISABLE_OLE1DDE))) {
        IFileOpenDialog *dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                       CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&dialog)))) {
          COMDLG_FILTERSPEC filters[] = {{L"scrcpy executable", L"scrcpy.exe"},
                                         {L"Executables", L"*.exe"}};
          dialog->SetFileTypes(2, filters);
          dialog->SetTitle(L"Select scrcpy.exe");
          dialog->SetOptions(FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST |
                             FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
          if (SUCCEEDED(dialog->Show(owner))) {
            IShellItem *item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
              PWSTR path = nullptr;
              if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                selected = path;
                CoTaskMemFree(path);
              }
              item->Release();
            }
          }
          dialog->Release();
        }
        CoUninitialize();
      }
      post(weak, [selected](Controller &c) {
        c.picking = false;
        if (!selected.empty() && c.scrcpy->set_path(selected)) {
          c.path_changed();
          c.status("scrcpy path saved.");
        }
        c.show();
      });
    }).detach();
  }
  bool apply_shortcut(const std::string &value, bool persist) {
    UINT modifiers = 0, key = 0;
    std::string canonical;
    if (!app::parse_hotkey(value, modifiers, key, canonical)) {
      status("Use modifiers plus A–Z, 0–9 or F1–F24 (for example Ctrl + Alt + "
             "S).");
      return false;
    }
    if (hotkey_id && canonical == settings.shortcut) {
      ui->set_shortcut_edit(slint::SharedString(canonical));
      status("Shortcut is already active.");
      return true;
    }
    int new_id = hotkey_id == 1 ? 2 : 1;
    if (!RegisterHotKey(tray, new_id, modifiers | MOD_NOREPEAT, key)) {
      status("Shortcut unavailable; another app may be using it. Choose a "
             "different combination.");
      log("[shortcut] " + app::system_error() + "\n");
      return false;
    }
    auto previous = settings.shortcut;
    settings.shortcut = canonical;
    if (persist && !save()) {
      settings.shortcut = previous;
      UnregisterHotKey(tray, new_id);
      return false;
    }
    if (hotkey_id)
      UnregisterHotKey(tray, hotkey_id);
    hotkey_id = new_id;
    ui->set_shortcut(slint::SharedString(canonical));
    ui->set_shortcut_edit(slint::SharedString(canonical));
    if (persist)
      status("Quick Mirror shortcut saved.");
    return true;
  }
  void save_update_settings() {
    if (ui->get_updating()) {
      status("Wait for the update operation to finish before changing its "
             "settings.");
      return;
    }
    auto previous = settings.auto_update;
    settings.auto_update = ui->get_auto_update();
    if (!save()) {
      settings.auto_update = previous;
      ui->set_auto_update(settings.auto_update);
      return;
    }
    status("Update settings saved.");
    if (settings.auto_update)
      update(false);
  }
  void update(bool install) {
    if (ui->get_updating())
      return;
    if (install &&
        (!sessions.empty() || picking || ui->get_scanning() || pending_quick)) {
      status("Stop mirroring and wait for device discovery before updating.");
      return;
    }
    if (install && available_version.empty())
      return;
    auto script = app::executable_directory() / "update.ps1";
    if (!std::filesystem::exists(script)) {
      ui->set_update_status(
          "update.ps1 is missing from the application folder.");
      return;
    }
    ui->set_updating(true);
    installing = install;
    auto_install_attempted = install;
    ui->set_update_status(install ? "Downloading and verifying the GUI update…"
                                  : "Checking GitHub releases…");
    auto expected = available_version;
    auto weak = weak_from_this();
    std::thread([weak, script, expected, install] {
      std::vector<std::wstring> args{L"-NoProfile",
                                     L"-NonInteractive",
                                     L"-ExecutionPolicy",
                                     L"Bypass",
                                     L"-File",
                                     script.wstring(),
                                     L"-Mode",
                                     install ? L"Stage" : L"Check",
                                     L"-Repository",
                                     app::wide(app::update_repository),
                                     L"-CurrentVersion",
                                     app::wide(APP_VERSION),
                                     L"-ExpectedVersion",
                                     app::wide(expected),
                                     L"-Architecture",
#if defined(_M_ARM64)
                                     L"arm64"
#elif defined(_WIN64)
                                     L"x64"
#else
                                     L"x86"
#endif
      };
      auto result =
          app::run_process(powershell(), args, {}, install ? 240000 : 45000);
      post(weak, [result, install](Controller &c) {
        c.ui->set_updating(false);
        c.installing = false;
        std::istringstream lines(result.text);
        std::string line, marker, value;
        while (std::getline(lines, line)) {
          line = app::trim(line);
          if (line.rfind("__", 0) == 0) {
            auto split = line.find('|');
            marker = line.substr(0, split);
            value = split == std::string::npos ? "" : line.substr(split + 1);
          } else if (!line.empty())
            c.log(line + "\n");
        }
        if (result.code != 0) {
          c.ui->set_update_status("Update failed. See Logs for details.");
          return;
        }
        if (marker == "__NONE__") {
          c.available_version.clear();
          c.ui->set_update_available(false);
          c.ui->set_update_status(slint::SharedString(value));
        } else if (marker == "__AVAILABLE__") {
          c.available_version = value;
          c.ui->set_update_available(true);
          c.ui->set_update_status(
              slint::SharedString("GUI " + value + " is available."));
          if (c.settings.auto_update && c.sessions.empty() && !c.picking &&
              !c.ui->get_scanning() && !c.pending_quick)
            c.update(true);
          else if (c.settings.auto_update)
            c.ui->set_update_status("Update available; installation waits "
                                    "until mirroring has stopped.");
        } else if (marker == "__READY__" && install)
          c.apply_update(app::wide(value));
        else
          c.ui->set_update_status(
              "Unexpected update response. See Logs for details.");
      });
    }).detach();
  }
  void apply_update(const std::filesystem::path &stage) {
    try {
      auto helper = stage / "apply-helper.ps1";
      std::filesystem::copy_file(
          app::executable_directory() / "update.ps1", helper,
          std::filesystem::copy_options::overwrite_existing);
      std::string error;
      if (!app::launch_detached(
              powershell(),
              {L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy",
               L"Bypass", L"-File", helper.wstring(), L"-Mode", L"Apply",
               L"-StageDirectory", stage.wstring(), L"-InstallDirectory",
               app::executable_directory().wstring(), L"-ParentProcessId",
               std::to_wstring(GetCurrentProcessId())},
              error)) {
        log(error + "\n");
        ui->set_update_status("Could not start the update helper.");
        return;
      }
      quit();
    } catch (const std::exception &e) {
      log(std::string(e.what()) + "\n");
      ui->set_update_status("Unable to stage update helper.");
    }
  }
  void initialize(bool updated, bool startup) {
    controller = this;
    settings_path = app::config_directory() / "settings.dat";
    std::string error;
    settings_writable = app::load_settings(settings_path, settings, error);
    if (!error.empty())
      log(error + "\n");
    ui->set_app_version(APP_VERSION);
    ui->set_auto_update(settings.auto_update);
    ui->set_selected_template(settings.default_template);
    publish_templates();
    edit_template(0);
    auto weak = weak_from_this();
    scrcpy = std::make_unique<Scrcpy>(
        [weak](std::string output) {
          post(weak,
               [output = std::move(output)](Controller &c) { c.log(output); });
        },
        [weak](std::string serial, bool running) {
          post(weak, [serial, running](Controller &c) {
            if (running)
              c.sessions.insert(serial);
            else
              c.sessions.erase(serial);
            c.publish_devices();
            if (!running && c.sessions.empty() && c.settings.auto_update &&
                !c.auto_install_attempted && c.ui->get_update_available())
              c.update(true);
          });
        });
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"ScrcpyTrayMsgWnd";
    RegisterClassW(&wc);
    tray = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, nullptr,
                           nullptr, instance, nullptr);
    icon.cbSize = sizeof(icon);
    icon.hWnd = tray;
    icon.uID = 1;
    icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    icon.uCallbackMessage = TrayMessage;
    icon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wcscpy_s(icon.szTip, L"scrcpy-gui · Quick Mirror");
    Shell_NotifyIconW(NIM_ADD, &icon);
    ui->set_shortcut_edit(slint::SharedString(settings.shortcut));
    if (!apply_shortcut(settings.shortcut, false))
      ui->set_shortcut("Shortcut unavailable");
    ui->on_refresh_devices([this] { refresh(); });
    ui->on_mirror_device(
        [this](slint::SharedString serial) { mirror(text(serial)); });
    ui->on_stop_device(
        [this](slint::SharedString serial) { scrcpy->stop(text(serial)); });
    ui->on_stop_all([this] { scrcpy->stop(); });
    ui->on_quick_mirror([this] { refresh(true); });
    ui->on_browse_scrcpy([this] { browse(); });
    ui->on_detect_scrcpy([this] {
      if (scrcpy->detect())
        path_changed();
      else
        status("scrcpy was not found on PATH. Use Browse to select it.");
    });
    ui->on_save_path([this] {
      if (scrcpy->set_path(app::wide(app::trim(text(ui->get_scrcpy_path()))))) {
        path_changed();
        status("scrcpy path saved.");
      } else
        status("Invalid executable path. Select an existing scrcpy.exe.");
    });
    ui->on_restart_adb([this, weak] {
      if (ui->get_scanning())
        return;
      ui->set_scanning(true);
      scrcpy->restart_adb([weak] {
        post(weak, [](Controller &c) {
          c.ui->set_scanning(false);
          c.refresh();
        });
      });
    });
    ui->on_save_shortcut(
        [this] { apply_shortcut(text(ui->get_shortcut_edit()), true); });
    ui->on_save_update_settings([this] { save_update_settings(); });
    ui->on_check_updates([this] { update(false); });
    ui->on_install_update([this] { update(true); });
    ui->on_select_template(
        [this](int index) { ui->set_selected_template(index); });
    ui->on_make_default([this] {
      int index = ui->get_selected_template();
      if (index < 0 || index >= (int)settings.templates.size())
        return;
      int previous = settings.default_template;
      settings.default_template = index;
      if (save()) {
        publish_templates();
        status("Default template saved for Quick Mirror.");
      } else
        settings.default_template = previous;
    });
    ui->on_edit_template([this](int index) { edit_template(index); });
    ui->on_new_template([this] {
      ui->set_editing_template(-1);
      ui->set_template_name("");
      ui->set_template_arguments("");
      status("Enter a name and scrcpy arguments, then save.");
    });
    ui->on_save_template([this] { save_template(); });
    ui->on_delete_template([this] { delete_template(); });
    ui->on_hide_to_tray([this] { hide(); });
    ui->on_quit_app([this] { quit(); });
    ui->window().on_close_requested([this] {
      visible = focused = false;
      hidden_at = GetTickCount64();
      return slint::CloseRequestResponse::HideWindow;
    });
    focus_timer.start(
        slint::TimerMode::Repeated, std::chrono::milliseconds(100), [this] {
          if (!visible || picking || ui->get_pinned())
            return;
          HWND hwnd = FindWindowW(nullptr, WindowTitle),
               fg = GetForegroundWindow();
          // Combo-box popups and the file picker owned by this process must not
          // dismiss the panel.
          DWORD pid = 0;
          GetWindowThreadProcessId(fg, &pid);
          if (hwnd && (fg == hwnd || pid == GetCurrentProcessId()))
            focused = true;
          else if (focused || GetTickCount64() - shown_at > 2000)
            hide();
        });
    device_timer.start(slint::TimerMode::Repeated, std::chrono::seconds(5),
                       [this] {
                         if (settings.auto_update && !auto_install_attempted &&
                             ui->get_update_available() && sessions.empty() &&
                             !picking && !ui->get_scanning() && !pending_quick)
                           update(true);
                         if (visible && ui->get_page() == 0 && !installing)
                           refresh(false, true);
                       });
    update_timer.start(slint::TimerMode::Repeated, std::chrono::hours(6),
                       [this] { update(false); });
    if (scrcpy->detect(settings.scrcpy_path)) {
      path_changed();
      if (!updated && !startup)
        pending_quick = true;
    } else {
      ui->set_page(1);
      if (!startup)
        show();
      status("Choose scrcpy.exe to get started. The app stays in your tray.");
    }
    if (updated) {
      wchar_t local[32768]{};
      GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
      std::ifstream result(std::filesystem::path(local) /
                           "scrcpy-pad/updates/result.txt");
      std::string message((std::istreambuf_iterator<char>(result)),
                          std::istreambuf_iterator<char>());
      log(message);
      status(app::trim(message));
      show();
    }
    // The repository may not have releases yet; this is a normal update state.
    update(false);
  }
  ~Controller() {
    if (tray)
      DestroyWindow(tray);
    controller = nullptr;
  }
};
LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!controller)
    return DefWindowProcW(hwnd, msg, wp, lp);
  static UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  if (msg == taskbar_created) {
    Shell_NotifyIconW(NIM_ADD, &controller->icon);
    return 0;
  }
  if (msg == TrayMessage) {
    if (lp == WM_LBUTTONUP) {
      controller->show(true);
      if (controller->visible)
        controller->refresh(false, true);
    } else if (lp == WM_RBUTTONUP) {
      POINT point;
      GetCursorPos(&point);
      HMENU menu = CreatePopupMenu();
      AppendMenuW(menu, MF_STRING, MirrorCommand, L"Quick Mirror");
      AppendMenuW(menu, MF_STRING, ShowCommand, L"Show / hide");
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, QuitCommand, L"Quit");
      SetForegroundWindow(hwnd);
      UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, point.x,
                                    point.y, 0, hwnd, nullptr);
      DestroyMenu(menu);
      PostMessageW(hwnd, WM_NULL, 0, 0);
      if (command == MirrorCommand)
        controller->refresh(true);
      else if (command == ShowCommand)
        controller->show(true);
      else if (command == QuitCommand)
        controller->quit();
    }
    return 0;
  }
  if (msg == WM_HOTKEY && (int)wp == controller->hotkey_id) {
    controller->refresh(true);
    return 0;
  }
  if (msg == WM_COPYDATA) {
    controller->show();
    controller->refresh(true);
    return 1;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

int main(int argc, char **argv) {
  bool startup = false, updated = false;
  for (int i = 1; i < argc; ++i) {
    startup |= std::string(argv[i]) == "--startup";
    updated |= std::string(argv[i]) == "--updated";
  }
  HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\ScrcpyPadSlint");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // Sign-in and installer launches must not activate an existing session.
    if (!startup) {
      if (HWND existing = FindWindowW(L"ScrcpyTrayMsgWnd", nullptr)) {
        COPYDATASTRUCT data{};
        SendMessageW(existing, WM_COPYDATA, 0, (LPARAM)&data);
      }
    }
    if (single)
      CloseHandle(single);
    return 0;
  }
  int result = 0;
  try {
    auto app = std::make_shared<Controller>();
    app->initialize(updated, startup);
    slint::run_event_loop(slint::EventLoopMode::RunUntilQuit);
  } catch (const std::exception &e) {
    MessageBoxW(nullptr, app::wide(e.what()).c_str(), L"scrcpy-gui",
                MB_OK | MB_ICONERROR);
    result = 1;
  }
  if (single)
    CloseHandle(single);
  return result;
}
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  return main(__argc, __argv);
}
