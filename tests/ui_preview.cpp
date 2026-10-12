#include "main.h"
#include "slint-platform.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>

class PreviewWindow : public slint::platform::WindowAdapter {
  slint::platform::SoftwareRenderer renderer_{slint::platform::SoftwareRenderer::RepaintBufferType::NewBuffer};
  slint::PhysicalSize size_{slint::PhysicalSize({420, 690})};
public:
  slint::PhysicalSize size() override { return size_; }
  void set_size(slint::PhysicalSize value) override { size_ = value; window().dispatch_resize_event(slint::LogicalSize({(float)value.width, (float)value.height})); }
  slint::platform::AbstractRenderer &renderer() override { return renderer_; }
};
class PreviewPlatform : public slint::platform::Platform {
public:
  std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override { return std::make_unique<PreviewWindow>(); }
};
int main(int argc, char **argv) {
  try {
    slint::platform::set_platform(std::make_unique<PreviewPlatform>());
    auto ui = MainWindow::create();
    ui->window().set_size(slint::PhysicalSize({420, 690}));
    DeviceRow pixel{}; pixel.serial = "USB123"; pixel.name = "Pixel 7 Pro"; pixel.detail = "USB · USB123"; pixel.ready = true;
    DeviceRow galaxy{}; galaxy.serial = "192.168.1.15:5555"; galaxy.name = "Galaxy S23"; galaxy.detail = "Wireless · 192.168.1.15:5555"; galaxy.ready = true;
    DeviceRow locked{}; locked.serial = "LOCKED"; locked.name = "Android device"; locked.detail = "USB · LOCKED · unauthorized"; locked.ready = false;
    ui->set_devices(std::make_shared<slint::VectorModel<DeviceRow>>(std::vector<DeviceRow>{pixel, galaxy, locked}));
    ui->set_template_names(std::make_shared<slint::VectorModel<slint::SharedString>>(std::vector<slint::SharedString>{"Default (1080p, 60fps)", "Lightweight (720p, 30fps)", "Full resolution"}));
    ui->set_template_name("Default (1080p, 60fps)"); ui->set_template_arguments("--max-size=1920 --max-fps=60");
    ui->set_scrcpy_path("C:\\Tools\\scrcpy\\scrcpy.exe"); ui->set_scrcpy_version("scrcpy 3.3.4");
    ui->set_update_status("No published release found (or repository is private).");
    ui->set_console_text("Ready.\n[adb] Found 3 device(s).\n[scrcpy] Found on PATH.\n");
    ui->set_status_text("Choose a device to mirror.");
    ui->show();
    std::filesystem::path output = argc > 1 ? argv[1] : "ui-previews";
    std::filesystem::create_directories(output);
    for (int page=0; page<4; ++page) {
      ui->set_page(page == 3 ? 0 : page);
      if (page == 3) ui->set_devices(std::make_shared<slint::VectorModel<DeviceRow>>());
      auto snapshot = ui->window().take_snapshot();
      if (!snapshot) throw std::runtime_error("Could not render preview");
      auto name = page == 0 ? "devices" : page == 1 ? "settings" : page == 2 ? "templates" : "empty";
      std::ofstream image(output / (std::string(name) + ".ppm"), std::ios::binary);
      image << "P6\n" << snapshot->width() << ' ' << snapshot->height() << "\n255\n";
      for (const auto &pixel : *snapshot) { image.put(pixel.r); image.put(pixel.g); image.put(pixel.b); }
      if (!image) throw std::runtime_error("Could not write preview");
    }
    // Exercise actual hit targets, rather than invoking callbacks directly.
    auto click = [&](float x, float y) {
      slint::platform::update_timers_and_animations();
      ui->window().dispatch_pointer_move_event(slint::LogicalPosition({x, y}));
      ui->window().dispatch_pointer_press_event(slint::LogicalPosition({x, y}), slint::PointerEventButton::Left);
      ui->window().dispatch_pointer_release_event(slint::LogicalPosition({x, y}), slint::PointerEventButton::Left);
    };
    click(300, 82);
    if (ui->get_page() != 1) throw std::runtime_error("Settings tab did not respond");
    ui->window().take_snapshot();
    ui->window().dispatch_pointer_scroll_event(slint::LogicalPosition({200, 310}), 0, -1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    slint::platform::update_timers_and_animations();
    auto settings = ui->window().take_snapshot();
    if (!settings) throw std::runtime_error("Could not render scrolled Settings");
    std::ofstream settings_image(output / "settings-updates.ppm", std::ios::binary);
    settings_image << "P6\n" << settings->width() << ' ' << settings->height() << "\n255\n";
    for (const auto &p : *settings) { settings_image.put(p.r); settings_image.put(p.g); settings_image.put(p.b); }
    click(335, 35);
    if (!ui->get_pinned()) throw std::runtime_error("Pin button did not respond");
    click(110, 82);
    if (ui->get_page() != 0) throw std::runtime_error("Devices tab did not respond");
    ui->set_devices(std::make_shared<slint::VectorModel<DeviceRow>>(std::vector<DeviceRow>{pixel, galaxy}));
    ui->window().take_snapshot();
    std::string mirrored;
    ui->on_mirror_device([&](slint::SharedString serial) { mirrored = serial.data(); });
    click(345, 186);
    if (mirrored != "USB123") throw std::runtime_error("Mirror button did not select its device");
    std::cout << "Rendered all views; tab switching, pinning and per-device Mirror hit targets passed.\n";
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
