# scrcpy-gui

A compact Windows tray app for [scrcpy](https://github.com/Genymobile/scrcpy), built with Slint and C++. The dark Devices / Settings interface follows the reference mockups in this repository.

## Use

Install the `scrcpy-gui-windows-<architecture>.exe` setup package to run automatically. Setup installs for your Windows account (no administrator rights needed), starts the app in the tray immediately, and registers it to start at every sign-in. Automatic launches stay in the tray without opening a popup or starting a mirror session. Click the tray icon to configure scrcpy on first use.

You can disable automatic startup in **Task Manager → Startup apps → scrcpy-gui**. **Quit** stops it for the current session; it starts again at the next sign-in unless disabled. Uninstalling through Windows **Installed apps** removes the startup entry and preserves your preferences. Quit the tray app before reinstalling or uninstalling. This is a background app in your Windows session, so it runs after sign-in rather than before login as a Windows service would.

The portable ZIP remains available for manual use and GUI updates. Extracting it or launching its executable does not register automatic startup.

1. Install scrcpy, including its adjacent `adb.exe`, or put both tools on PATH.
2. Launch `scrcpy-ui.exe`. The app restores your saved executable or detects `SCRCPY` / PATH. Use **Settings → Browse** if it cannot find scrcpy.
3. Enable USB debugging on your phone and accept its authorization prompt. Connected USB and wireless ADB devices appear in **Devices**; offline and unauthorized devices are shown but cannot be started.
4. Choose a template and click a device's **Mirror** button. **Stop** affects that device; **Stop all** affects only this app's mirror sessions.

**Start Default**, the tray's **Quick Mirror** action, and **Ctrl + Alt + S** use the saved default template. With one ready device they start it immediately; with several, the popup asks you to choose. Manual launches also attempt Quick Mirror when scrcpy is configured. Launching a second instance manually delegates Quick Mirror to the existing instance; automatic startup (`--startup`) leaves an existing instance alone.

The window hides when focus leaves it. **Pin** keeps it open. Closing the window hides it to the tray; use **Quit app** or the tray's **Quit** command to exit. Device discovery refreshes every five seconds while the Devices tab is visible. Logs are bounded to avoid unbounded memory growth.

## Settings and templates

- **Scrcpy executable:** browse, detect, or enter and save a path. ADB is resolved beside scrcpy first, then on PATH.
- **Shortcut:** enter modifiers (`Ctrl`, `Alt`, `Shift`, `Win`) plus a letter, digit, or `F1`–`F24`. Apply reports conflicts and preserves the previous registered shortcut when a new one fails.
- **Manage templates:** create, edit, and delete named sets of scrcpy command-line arguments. Quote paths containing spaces. Device-selection arguments are reserved for the Devices tab. Arguments are passed directly to the executable without a command shell.
- **Make default:** saves the selected template for startup and Quick Mirror. Per-device Mirror buttons use the currently selected template.

Settings are saved atomically in `%APPDATA%\scrcpy-tray\settings.dat`. The earlier `scrcpy_path.txt` selection is migrated automatically. At least one template must remain. A corrupt settings file is reported and is not silently overwritten.

The bundled presets use `--max-size=1920 --max-fps=60`, a lighter 1280px/30fps preset without audio, and full resolution. `--max-size` limits the longer edge; see [scrcpy video options](https://github.com/Genymobile/scrcpy/blob/master/doc/video.md) and [device selection](https://github.com/Genymobile/scrcpy/blob/master/doc/connection.md).

## GUI updates

Updates target **this GUI**, not scrcpy. The update repository is fixed to `Deference/scrcpy-gui-slint` by the shared `app::update_repository` constant. Settings provides automatic updates and manual check/install controls. No published release (or a private repository) is reported without treating it as an application failure.

The app checks the latest stable GitHub release at startup and every six hours. **Check updates** runs a manual check. **Auto-update this GUI from GitHub** is opt-in and persists; installation waits until mirroring and device discovery are idle. You can also use **Install & restart**. A failed automatic installation is retried after the next check, rather than in a rapid loop.

The updater downloads the matching architecture's portable ZIP, verifies GitHub's SHA-256 asset digest, validates the flat archive, and stages it under `%LOCALAPPDATA%\scrcpy-pad\updates`. A helper waits for the GUI to exit, backs up replaced files, installs the new executable/runtime, rolls back on copy failure, and restarts the GUI. Preferences remain in AppData. The install folder must be writable by the current user. Update results are recorded in `updates\result.txt`; previous staging/backup folders remain available there.

### Publish a release

1. Increase `project(... VERSION ...)` in `CMakeLists.txt` (for example, to `0.1.1`).
2. Build and test, then create the portable package:

   ```powershell
   cmake --build build --config Release
   ctest --test-dir build -C Release --output-on-failure
   cpack --config build/CPackConfig.cmake -C Release -B build/packages
   ```

3. Publish a stable GitHub release tagged `v0.1.1` in `Deference/scrcpy-gui-slint`. Attach `scrcpy-gui-windows-x64.zip` from `build/packages` without renaming it. ARM64 and x86 builds use `scrcpy-gui-windows-arm64.zip` and `scrcpy-gui-windows-x86.zip`.
4. The package is flat and includes `scrcpy-ui.exe`, `slint_cpp.dll`, MSVC runtime DLLs, `update.ps1`, and documentation. Upload through GitHub's release assets API/UI so GitHub supplies the asset `digest`; the updater refuses unverified assets.

To also publish the installer with automatic startup, install [NSIS 3.03 or newer](https://nsis.sourceforge.io/Download), then run:

```powershell
cpack --config build/CPackConfig.cmake -C Release -G NSIS -B build/packages
```

Attach the resulting `scrcpy-gui-windows-x64.exe` (or matching architecture) alongside the ZIP. Keep the ZIP attached: installed copies still use it for in-place updates, and their startup registration continues pointing to the same executable.

Nothing is published automatically by this project. The update repository needs a published release before a real update can be downloaded.

## Build on Windows

Requires CMake 3.21+, a C++ compiler with the Windows SDK (Visual Studio C++ tools), and Rust when Slint is built from source. CMake uses an installed Slint package if available; otherwise it fetches Slint. Run initial configuration from a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
.\build\app\scrcpy-ui.exe
```

For a Visual Studio generator, add `--config Release` and run `build\app\Release\scrcpy-ui.exe`. Keep the executable, its DLLs and `update.ps1` together. Quit older running builds before launching the new build.

Tests cover device parsing, settings persistence, argument quoting, shortcut validation, process failures/cancellation, mocked multi-device mirroring, quiet startup and manual activation, and updater fixtures. When NSIS is found during configuration, installer fixtures also verify automatic launch, startup registration, reinstall/uninstall cleanup, and running-app failure paths using temporary test entries. Quit the tray app before testing; startup and installer checks skip if it is already running. When Slint's software renderer is available, `scrcpy-ui-preview` renders Devices, Settings, Templates and empty states without opening native windows or accessing a phone; PPM previews are written to `build/ui-previews`.

Zed's project configuration supplies the `@material` library path for components that import it. The current popup uses custom Slint controls plus standard input/scroll widgets.
