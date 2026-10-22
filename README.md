# scrcpy-gui

A compact Windows tray app for [scrcpy](https://github.com/Genymobile/scrcpy), built with C++ and Slint. Mirror and control Android devices, manage scrcpy presets, and start sessions with a keyboard shortcut.

Currently supports **Windows only**. Install scrcpy and ADB separately; they are not bundled with this app.

## Use

1. Install scrcpy with its included `adb.exe`.
2. Install the GUI setup package, or extract the portable ZIP and run `scrcpy-ui.exe`. The installer starts the app in the tray and enables startup at sign-in.
3. Enable USB debugging on your Android device, connect it, and accept the authorization prompt.
4. Open the tray icon. In **Settings**, browse to `scrcpy.exe` if it was not detected automatically.
5. In **Devices**, choose a template and click **Mirror** next to your device.

Use **Make default** to choose the preset for **Start Default** and **Quick Mirror**. The default shortcut is **Ctrl + Alt + S**; with multiple ready devices, the app asks you to choose one.

Closing the window hides it to the tray. **Pin** keeps it open when focus changes; **Quit app** exits. Portable copies do not enable startup at sign-in.

## Features

- Discover USB and wireless devices connected through ADB, with connection and authorization status.
- Mirror multiple devices and stop sessions individually or together.
- Create, edit, and save scrcpy argument templates, including resolution, frame rate, and audio preferences.
- Customize the global Quick Mirror shortcut.
- Keep the app in the tray, with optional window pinning and automatic startup through the installer.
- View logs, detect scrcpy on PATH, and restart ADB from Settings.
- Check for GUI updates from `DeFerence3/scrcpy-gui-slint`, or enable automatic updates. These updates do not replace scrcpy.

Preferences are stored in `%APPDATA%\scrcpy-tray\settings.dat`.

## Build locally

Requirements: Windows, Git, CMake 3.21+, Ninja, and Visual Studio C++ build tools with the Windows SDK. Install stable Rust if building Slint from source. CMake uses an installed Slint package when available; otherwise it downloads and builds Slint.

From the repository directory in **Visual Studio Developer PowerShell**:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DSLINT_FEATURE_BACKEND_QT=OFF `
    -DSLINT_FEATURE_RENDERER_SKIA=OFF `
    -DSLINT_FEATURE_RENDERER_SOFTWARE=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
.\build\app\scrcpy-ui.exe
```

Quit any running copy before testing or launching the new build. Keep the executable, runtime DLLs, and `update.ps1` together.

If your build directory uses a Visual Studio generator, build with `--config Release`, test with `-C Release`, and launch `.\build\app\Release\scrcpy-ui.exe`.

To create distribution packages:

```powershell
# Portable ZIP
cpack --config build/CPackConfig.cmake -C Release -G ZIP -B build/packages

# Installer (requires NSIS)
cpack --config build/CPackConfig.cmake -C Release -G NSIS -B build/packages
```

Packages are written to `build/packages`. To set the app and package version, add `"-DSCRCPY_VERSION=0.1.1"` when configuring CMake.

## Publish a release

Push a new version tag on the commit to release:

```powershell
git tag v0.0.3
git push origin v0.0.3
```

Use `vMAJOR.MINOR.PATCH` (or `vMAJOR.MINOR.PATCH.REVISION`) and a version higher than the previous release. The distribution workflow builds and tests Windows x64, packages the portable ZIP and installer, then publishes a GitHub release with both packages and `SHA256SUMS.txt`. The app version comes from the tag. Publishing uses the repository's `PAT` Actions secret, which must have write access to repository contents (or the `repo` scope for a classic PAT).

The app checks the latest published, non-prerelease GitHub release and offers an update only when its version is newer. Keep the asset named `scrcpy-gui-windows-x64.zip`; the updater verifies its GitHub SHA-256 digest before installation. Workflow artifacts alone are not visible to the updater.

To test update detection locally, configure a build with a lower version, such as `"-DSCRCPY_VERSION=0.0.1"`, rebuild, and use **Settings → Check updates**. Builds made before the repository address was corrected must first be rebuilt or manually replaced with a current release.
