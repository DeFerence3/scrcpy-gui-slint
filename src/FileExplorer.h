#pragma once
#include "main.h"
#include "DeviceFiles.h"

class FileExplorer : public std::enable_shared_from_this<FileExplorer> {
public:
  static std::shared_ptr<FileExplorer> create(slint::ComponentHandle<MainWindow> ui);
  ~FileExplorer();
  void attach(HWND hwnd);
  void detach();
  void open(HWND hwnd, const std::wstring &adb, const std::string &serial, const std::string &name);
  bool busy() const { return ui_->get_file_busy(); }
  void shutdown();
  void set_installing(bool value) { installing_ = value; }
private:
  explicit FileExplorer(slint::ComponentHandle<MainWindow> ui) : ui_(std::move(ui)) {}
  slint::ComponentHandle<MainWindow> ui_;
  std::wstring adb_;
  std::string serial_, folder_ = "/sdcard";
  std::vector<app::DeviceFile> files_;
  std::shared_ptr<app::Cancel> cancel_;
  HWND hwnd_ = nullptr;
  bool registered_ = false, closed_ = false, installing_ = false;
  static void post(std::weak_ptr<FileExplorer> weak, std::function<void(FileExplorer &)> callback);
  void navigate(const std::string &path, const std::string &message = {}, bool failed = false);
  void upload(std::vector<std::filesystem::path> paths);
  void pick_upload();
  void download(int index);
  void finish(const std::string &error, const std::string &success, bool refresh);
  void set_status(const std::string &message, bool failed = false);
};
