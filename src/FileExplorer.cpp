#include "FileExplorer.h"
#include "FileDropTarget.h"
#include <algorithm>
#include <iomanip>
#include <shellapi.h>
#include <shobjidl.h>
#include <sstream>
#include <set>
#include <thread>

namespace {
std::string text(const slint::SharedString &value) { return {value.data(), value.size()}; }
std::string file_size(uint64_t bytes) {
  double size = static_cast<double>(bytes);
  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  int unit = 0;
  while (size >= 1024 && unit < 4) { size /= 1024; ++unit; }
  std::ostringstream value;
  value << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << size << ' ' << units[unit];
  return value.str();
}
std::filesystem::path dialog_path(IFileDialog *dialog) {
  std::filesystem::path result;
  IShellItem *item = nullptr;
  if (SUCCEEDED(dialog->GetResult(&item))) {
    PWSTR value = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) {
      result = value; CoTaskMemFree(value);
    }
    item->Release();
  }
  return result;
}
}

void FileExplorer::post(std::weak_ptr<FileExplorer> weak, std::function<void(FileExplorer &)> callback) {
  slint::invoke_from_event_loop([weak, callback = std::move(callback)] {
    if (auto self = weak.lock(); self && !self->closed_) callback(*self);
  });
}
std::shared_ptr<FileExplorer> FileExplorer::create(slint::ComponentHandle<MainWindow> ui) {
  auto self = std::shared_ptr<FileExplorer>(new FileExplorer(std::move(ui)));
  auto weak = std::weak_ptr<FileExplorer>(self);
  self->ui_->on_file_navigate([weak](slint::SharedString path) { if (auto s = weak.lock()) s->navigate(text(path)); });
  self->ui_->on_file_up([weak] { if (auto s = weak.lock()) s->navigate(app::device_parent(s->folder_)); });
  self->ui_->on_file_refresh([weak] { if (auto s = weak.lock()) s->navigate(s->folder_); });
  self->ui_->on_file_open_folder([weak](int index) {
    if (auto s = weak.lock(); s && !s->busy() && index >= 0 && index < (int)s->files_.size() && s->files_[index].directory)
      s->navigate(app::device_child(s->folder_, s->files_[index].name));
  });
  self->ui_->on_file_download([weak](int index) { if (auto s = weak.lock()) s->download(index); });
  self->ui_->on_file_upload([weak] { if (auto s = weak.lock()) s->pick_upload(); });
  self->ui_->on_file_cancel([weak] {
    if (auto s = weak.lock(); s && s->cancel_) { s->cancel_->requested = true; s->set_status("Cancelling…"); }
  });
  return self;
}
FileExplorer::~FileExplorer() { shutdown(); }
void FileExplorer::shutdown() {
  closed_ = true;
  if (cancel_) cancel_->requested = true;
  detach();
}
void FileExplorer::detach() {
  if (registered_ && IsWindow(hwnd_)) RevokeDragDrop(hwnd_);
  registered_ = false;
  hwnd_ = nullptr;
  ui_->set_file_dragging(false);
}
void FileExplorer::set_status(const std::string &message, bool failed) {
  ui_->set_file_failed(failed);
  ui_->set_file_status(slint::SharedString(message));
}
void FileExplorer::attach(HWND hwnd) {
  if (closed_ || !hwnd || (hwnd == hwnd_ && registered_)) return;
  if (registered_ && IsWindow(hwnd_)) RevokeDragDrop(hwnd_);
  hwnd_ = hwnd;
  RevokeDragDrop(hwnd_);
  auto weak = weak_from_this();
  auto target = app::create_file_drop_target(
    [weak] { auto s = weak.lock(); return s && !s->closed_ && !s->installing_ && !s->busy() && s->ui_->get_page() == 3 && s->ui_->get_file_ready(); },
    [weak](bool hovering) { if (auto s = weak.lock()) s->ui_->set_file_dragging(hovering); },
    [weak](std::vector<std::filesystem::path> paths) {
      // Leave the native drop callback before starting dialogs or transfers.
      if (auto s = weak.lock()) {
        post(weak, [paths = std::move(paths), folder = s->folder_, serial = s->serial_](FileExplorer &current) {
          if (folder == current.folder_ && serial == current.serial_) current.upload(paths);
          else current.set_status("The open folder changed. Drop the files again.", true);
        });
      }
    });
  registered_ = SUCCEEDED(RegisterDragDrop(hwnd_, target));
  target->Release();
}
void FileExplorer::open(HWND hwnd, const std::wstring &adb, const std::string &serial, const std::string &name) {
  attach(hwnd);
  ui_->set_page(3);
  if (busy()) {
    if (serial != serial_) set_status("Finish or cancel the current operation before switching devices.");
    return;
  }
  bool changed = serial_ != serial;
  adb_ = adb; serial_ = serial;
  ui_->set_file_device_name(slint::SharedString(name));
  ui_->set_file_device_serial(slint::SharedString(serial));
  navigate(changed ? "/sdcard" : folder_);
}
void FileExplorer::navigate(const std::string &path, const std::string &message, bool failed) {
  if (busy()) return;
  if (installing_) { set_status("Wait for the GUI update to finish."); return; }
  std::string target;
  try { target = app::device_path(path); }
  catch (const std::exception &e) { ui_->set_file_path(slint::SharedString(folder_)); set_status(e.what(), true); return; }
  ui_->set_file_busy(true); ui_->set_file_ready(false);
  ui_->set_file_path(slint::SharedString(target));
  files_.clear(); ui_->set_file_entries(std::make_shared<slint::VectorModel<FileRow>>());
  set_status(message.empty() ? "Loading " + target + "…" : message);
  cancel_ = std::make_shared<app::Cancel>();
  auto weak = weak_from_this();
  std::thread([weak, adb = adb_, serial = serial_, target, message, failed, cancel = cancel_] {
    auto listing = app::list_device_files(adb, serial, target, cancel);
    post(weak, [listing = std::move(listing), target, message, failed](FileExplorer &s) {
      s.ui_->set_file_busy(false); s.cancel_.reset();
      s.folder_ = target;
      if (!listing.error.empty()) { s.set_status(listing.error, true); return; }
      s.files_ = listing.files;
      std::vector<FileRow> rows;
      for (const auto &file : s.files_) {
        FileRow row; row.name = slint::SharedString(file.name); row.directory = file.directory;
        row.detail = slint::SharedString(file.directory ? "Folder" : file_size(file.size)); rows.push_back(row);
      }
      s.ui_->set_file_entries(std::make_shared<slint::VectorModel<FileRow>>(rows));
      s.ui_->set_file_ready(true);
      s.set_status(message.empty() ? std::to_string(rows.size()) + " items · " + target : message, failed);
      if (!s.registered_ && s.hwnd_) s.set_status("Drag and drop is unavailable. Use the upload icon.", true);
    });
  }).detach();
}
void FileExplorer::finish(const std::string &error, const std::string &success, bool refresh) {
  ui_->set_file_uploading(false);
  ui_->set_file_busy(false); cancel_.reset();
  // A failed batch can still have copied earlier files. Refresh those too.
  if (refresh) navigate(folder_, error.empty() ? success : error, !error.empty());
  else set_status(error.empty() ? success : error, !error.empty());
}
void FileExplorer::upload(std::vector<std::filesystem::path> paths) {
  if (ui_->get_page() != 3 || busy() || !ui_->get_file_ready() || paths.empty()) return;
  if (installing_) { set_status("Wait for the GUI update to finish."); return; }
  std::vector<std::string> collisions;
  std::set<std::string> names;
  for (const auto &path : paths) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) { set_status("Drop files only; folders cannot be uploaded.", true); return; }
    auto name = app::utf8(path.filename().wstring());
    if (!names.insert(name).second) { set_status("Two selected files have the same name: " + name + ". Rename one first.", true); return; }
    auto existing = std::find_if(files_.begin(), files_.end(), [&](const auto &file) { return file.name == name; });
    if (existing != files_.end()) {
      if (existing->directory) { set_status("A folder named " + name + " already exists. Rename the local file first.", true); return; }
      collisions.push_back(name);
    }
  }
  if (!collisions.empty()) {
    std::string prompt = "Replace these files in " + folder_ + "?\n";
    for (size_t i = 0; i < std::min<size_t>(collisions.size(), 8); ++i) prompt += "\n" + collisions[i];
    if (collisions.size() > 8) prompt += "\n…";
    // Keep the device and destination fixed while the modal prompt pumps events.
    ui_->set_file_busy(true);
    auto answer = MessageBoxW(hwnd_, app::wide(prompt).c_str(), L"Replace device files", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    ui_->set_file_busy(false);
    if (answer != IDYES || closed_) return;
  }
  ui_->set_file_path(slint::SharedString(folder_)); // An unsubmitted path edit is not the upload destination.
  ui_->set_file_busy(true); set_status("Preparing upload…");
  ui_->set_file_uploading(true);
  cancel_ = std::make_shared<app::Cancel>();
  auto weak = weak_from_this();
  std::thread([weak, adb = adb_, serial = serial_, folder = folder_, paths = std::move(paths), cancel = cancel_] {
    auto error = app::upload_device_files(adb, serial, folder, paths, cancel, [weak](std::string message) {
      post(weak, [message](FileExplorer &s) { s.set_status(message); });
    });
    post(weak, [error, count = paths.size(), folder](FileExplorer &s) {
      s.finish(error, "Uploaded " + std::to_string(count) + " file(s) to " + folder + ".", true);
    });
  }).detach();
}
void FileExplorer::pick_upload() {
  if (ui_->get_page() != 3 || busy() || !ui_->get_file_ready()) return;
  if (installing_) { set_status("Wait for the GUI update to finish."); return; }
  // Native dialogs run on the UI's STA and pump messages. Busy prevents a second
  // operation or device switch while the selection is pending.
  ui_->set_file_busy(true);
  IFileOpenDialog *dialog = nullptr;
  std::vector<std::filesystem::path> paths;
  HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
  if (SUCCEEDED(result)) {
    dialog->SetTitle(L"Upload files to the open device folder");
    dialog->SetOptions(FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    result = dialog->Show(hwnd_);
    if (SUCCEEDED(result)) {
      IShellItemArray *items = nullptr;
      if (SUCCEEDED(dialog->GetResults(&items))) {
        DWORD count = 0; items->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
          IShellItem *item = nullptr;
          if (SUCCEEDED(items->GetItemAt(i, &item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { paths.emplace_back(path); CoTaskMemFree(path); }
            item->Release();
          }
        }
        items->Release();
      }
    }
    dialog->Release();
  }
  ui_->set_file_busy(false);
  if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_CANCELLED)) set_status("Could not open the file picker.", true);
  if (!closed_) upload(std::move(paths));
}
void FileExplorer::download(int index) {
  if (ui_->get_page() != 3 || busy() || !ui_->get_file_ready() || index < 0 || index >= (int)files_.size() || files_[index].directory) return;
  if (installing_) { set_status("Wait for the GUI update to finish."); return; }
  auto remote = app::device_child(folder_, files_[index].name);
  auto name = app::wide(files_[index].name);
  for (auto &ch : name) if (ch < 32 || std::wstring(L"<>:\"/\\|?*").find(ch) != std::wstring::npos) ch = L'_';
  while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
  if (name.empty()) name = L"download";
  ui_->set_file_busy(true);
  IFileSaveDialog *dialog = nullptr;
  std::filesystem::path local;
  HRESULT result = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
  if (SUCCEEDED(result)) {
    dialog->SetTitle(L"Download device file"); dialog->SetFileName(name.c_str());
    dialog->SetOptions(FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    result = dialog->Show(hwnd_);
    if (SUCCEEDED(result)) local = dialog_path(dialog);
    dialog->Release();
  }
  if (local.empty() || closed_) {
    ui_->set_file_busy(false);
    if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_CANCELLED)) set_status("Could not open the Save As dialog.", true);
    return;
  }
  set_status("Downloading " + files_[index].name + "…");
  cancel_ = std::make_shared<app::Cancel>();
  auto weak = weak_from_this();
  std::thread([weak, adb = adb_, serial = serial_, remote, local, cancel = cancel_] {
    auto error = app::download_device_file(adb, serial, remote, local, cancel);
    post(weak, [error, local](FileExplorer &s) { s.finish(error, "Saved to " + app::utf8(local.wstring()), false); });
  }).detach();
}
