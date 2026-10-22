# Repository Guidelines

## Project Structure & Module Organization

This Windows tray app wraps scrcpy using C++17 and Slint.

- `src/main.cpp` connects the UI, tray, hotkeys, and update workflow. `AppCore.*` handles settings and device parsing; `Platform.*` provides Windows/process helpers; `Scrcpy.*` manages ADB discovery and mirror sessions.
- `ui/main.slint` defines the interface, reusable controls, and theme. No separate asset directory is present.
- `tests/` contains core/backend checks, a mock tool executable, updater fixtures, and headless UI previews.
- `scripts/update.ps1` implements updates; `cmake/` contains the MSVC environment helper. Root `CMakeLists.txt` defines targets, tests, and packaging.
- Keep generated code, binaries, and fetched dependencies inside build directories; do not commit them.

## Build, Test, and Development Commands

Use Visual Studio Developer PowerShell with CMake 3.21+, Ninja, Visual Studio C++ tools, and the Windows SDK. Rust is required when CMake builds Slint from source instead of using an installed package.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
.\build\app\scrcpy-ui.exe
cpack --config build/CPackConfig.cmake -C Release -B build/packages
```

These configure, build, test, launch, and package the app. For Visual Studio generators, build with `--config Release`, test with `-C Release`, and launch `build\app\Release\scrcpy-ui.exe`. Keep runtime DLLs and `update.ps1` beside the executable.

## Coding Style & Naming Conventions

Match surrounding formatting: C++ generally uses two-space indentation; Slint and PowerShell use four spaces. Use `PascalCase` types/components, `snake_case` C++ functions and variables, and kebab-case Slint properties. Headers use `#pragma once`. No repository formatter or linter configuration is present; avoid unrelated reformatting.

## Testing Guidelines

CTest runs standalone C++ assertion helpers and PowerShell fixtures, without a third-party test framework or enforced coverage threshold. Extend `tests/*_tests.cpp` or `tests/updater_tests.ps1` for behavior changes, including failure paths. Use `tool_fixture.cpp` for simulated processes/devices. With Slint's software renderer enabled, `scrcpy-ui-preview` produces images in `build/ui-previews`; inspect these for UI changes.

## Commit & Pull Request Guidelines

History contains only `initial commit`, so no established message convention exists. Use concise imperative subjects, such as `Fix device discovery cancellation`. Keep commits focused. PRs should explain the behavior change, link relevant issues, report validation commands/results, and include screenshots or previews for UI changes.
