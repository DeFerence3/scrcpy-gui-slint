cmake_minimum_required(VERSION 3.21)
project(scrcpy-ui VERSION 0.1.0 LANGUAGES CXX)

find_package(Slint QUIET)
if (NOT Slint_FOUND)
  message("Slint could not be located in the CMake module search path. Downloading it from Git and building it locally")
  include(FetchContent)
  FetchContent_Declare(
    Slint
    GIT_REPOSITORY https://github.com/slint-ui/slint.git
    # `release/1` will auto-upgrade to the latest Slint >= 1.0.0 and < 2.0.0
    # `release/1.0` will auto-upgrade to the latest Slint >= 1.0.0 and < 1.1.0
    GIT_TAG release/1
    SOURCE_SUBDIR api/cpp
  )
  FetchContent_MakeAvailable(Slint)
endif (NOT Slint_FOUND)

add_library(scrcpy-core STATIC src/AppCore.cpp src/Platform.cpp)
target_compile_features(scrcpy-core PUBLIC cxx_std_17)
target_include_directories(scrcpy-core PUBLIC src)
target_link_libraries(scrcpy-core PUBLIC shell32)
add_executable(scrcpy-ui WIN32 src/main.cpp src/Scrcpy.cpp)
set_target_properties(scrcpy-ui PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/app")
target_compile_features(scrcpy-ui PRIVATE cxx_std_17)
target_compile_definitions(scrcpy-ui PRIVATE APP_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(scrcpy-core PUBLIC /utf-8)
endif()

# Ninja remembers cl.exe, but does not retain the Developer Prompt's SDK
# environment. Restore it for compilation and linking from ordinary shells.
if (MSVC AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
  get_filename_component(MSVC_SEARCH_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  while (MSVC_SEARCH_DIR)
    if (EXISTS "${MSVC_SEARCH_DIR}/Common7/Tools/VsDevCmd.bat")
      set(MSVC_DEV_CMD "${MSVC_SEARCH_DIR}/Common7/Tools/VsDevCmd.bat")
      break()
    endif()
    get_filename_component(MSVC_PARENT_DIR "${MSVC_SEARCH_DIR}" DIRECTORY)
    if (MSVC_PARENT_DIR STREQUAL MSVC_SEARCH_DIR)
      break()
    endif()
    set(MSVC_SEARCH_DIR "${MSVC_PARENT_DIR}")
  endwhile()
  if (MSVC_DEV_CMD AND CMAKE_CXX_COMPILER MATCHES "/bin/Host([^/]+)/([^/]+)/cl.exe$")
    set(MSVC_HOST_ARCH "${CMAKE_MATCH_1}")
    set(MSVC_TARGET_ARCH "${CMAKE_MATCH_2}")
    configure_file(cmake/msvc-env.cmd.in msvc-env.cmd @ONLY)
    set_target_properties(scrcpy-ui PROPERTIES
      CXX_COMPILER_LAUNCHER "${CMAKE_CURRENT_BINARY_DIR}/msvc-env.cmd"
      CXX_LINKER_LAUNCHER "${CMAKE_CURRENT_BINARY_DIR}/msvc-env.cmd")
    set_target_properties(scrcpy-core PROPERTIES
      CXX_COMPILER_LAUNCHER "${CMAKE_CURRENT_BINARY_DIR}/msvc-env.cmd")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_CURRENT_BINARY_DIR}/msvc-env.cmd")
    set(CMAKE_CXX_LINKER_LAUNCHER "${CMAKE_CURRENT_BINARY_DIR}/msvc-env.cmd")
  endif()
endif()

add_library(scrcpy-widgets STATIC)
target_link_libraries(scrcpy-widgets PUBLIC Slint::Slint)
if(MSVC)
  target_compile_options(scrcpy-widgets PRIVATE /utf-8)
endif()
slint_target_sources(scrcpy-widgets ui/main.slint LIBRARY_PATHS material=${CMAKE_CURRENT_SOURCE_DIR}/material-1.0/material.slint)
target_link_libraries(scrcpy-ui PRIVATE scrcpy-widgets scrcpy-core ole32 uuid)
# On Windows, copy the Slint DLL next to the application binary so that it's found.
if (WIN32)
    add_custom_command(TARGET scrcpy-ui POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:scrcpy-ui> $<TARGET_FILE_DIR:scrcpy-ui> COMMAND_EXPAND_LISTS)
endif()

configure_file(scripts/update.ps1 app/update.ps1 COPYONLY)
add_custom_command(TARGET scrcpy-ui POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/update.ps1" "$<TARGET_FILE_DIR:scrcpy-ui>/update.ps1")
include(CTest)
if(BUILD_TESTING)
  add_executable(scrcpy-core-tests tests/core_tests.cpp)
  target_link_libraries(scrcpy-core-tests PRIVATE scrcpy-core)
  add_test(NAME scrcpy-core COMMAND scrcpy-core-tests)
  add_executable(scrcpy-tool-fixture tests/tool_fixture.cpp)
  target_link_libraries(scrcpy-tool-fixture PRIVATE scrcpy-core)
  add_executable(scrcpy-backend-tests tests/backend_tests.cpp src/Scrcpy.cpp)
  target_link_libraries(scrcpy-backend-tests PRIVATE scrcpy-core)
  add_test(NAME scrcpy-backend COMMAND scrcpy-backend-tests $<TARGET_FILE:scrcpy-tool-fixture>)
  add_executable(scrcpy-startup-tests tests/startup_tests.cpp)
  target_link_libraries(scrcpy-startup-tests PRIVATE scrcpy-core)
  add_dependencies(scrcpy-startup-tests scrcpy-ui scrcpy-tool-fixture)
  add_test(NAME scrcpy-startup COMMAND scrcpy-startup-tests $<TARGET_FILE:scrcpy-ui>
    $<TARGET_FILE:scrcpy-tool-fixture> "${CMAKE_BINARY_DIR}/startup-tests")
  set_tests_properties(scrcpy-startup PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 30)
  find_program(POWERSHELL_EXECUTABLE powershell)
  if(POWERSHELL_EXECUTABLE)
    add_test(NAME scrcpy-updater COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
      -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/updater_tests.ps1"
      -UpdateScript "${CMAKE_CURRENT_SOURCE_DIR}/scripts/update.ps1"
      -Fixture $<TARGET_FILE:scrcpy-tool-fixture> -WorkDirectory "${CMAKE_BINARY_DIR}/updater-tests")
    find_program(SCRCPY_MAKENSIS_EXECUTABLE makensis
      HINTS "$ENV{ProgramFiles}/NSIS" "$ENV{ProgramFiles\(x86\)}/NSIS"
      PATH_SUFFIXES Bin)
    if(SCRCPY_MAKENSIS_EXECUTABLE)
      add_test(NAME scrcpy-installer COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer_tests.ps1"
        -Template "${CMAKE_CURRENT_SOURCE_DIR}/cmake/NSIS.template.in"
        -MakeNsis "${SCRCPY_MAKENSIS_EXECUTABLE}" -Fixture $<TARGET_FILE:scrcpy-tool-fixture>
        -WorkDirectory "${CMAKE_BINARY_DIR}/installer-tests")
      set_tests_properties(scrcpy-installer PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 90)
    endif()
  endif()
  if(SLINT_FEATURE_RENDERER_SOFTWARE)
    add_executable(scrcpy-ui-preview tests/ui_preview.cpp)
    set_target_properties(scrcpy-ui-preview PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    target_link_libraries(scrcpy-ui-preview PRIVATE scrcpy-widgets)
    if(MSVC)
      target_compile_options(scrcpy-ui-preview PRIVATE /utf-8)
    endif()
    add_custom_command(TARGET scrcpy-ui-preview POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy
      $<TARGET_RUNTIME_DLLS:scrcpy-ui-preview> $<TARGET_FILE_DIR:scrcpy-ui-preview> COMMAND_EXPAND_LISTS)
    add_test(NAME scrcpy-ui-preview COMMAND scrcpy-ui-preview "${CMAKE_BINARY_DIR}/ui-previews")
  endif()
endif()

install(TARGETS scrcpy-ui RUNTIME DESTINATION . COMPONENT gui)
install(FILES $<TARGET_RUNTIME_DLLS:scrcpy-ui> DESTINATION . COMPONENT gui)
install(FILES scripts/update.ps1 README.md LICENSE.txt DESTINATION . COMPONENT gui)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT gui)
include(InstallRequiredSystemLibraries)
set(CPACK_GENERATOR ZIP)
# Keep the portable ZIP for the updater; `cpack -G NSIS` builds the installer.
# Our small template uses HKCU consistently, even when run by an administrator.
set(CPACK_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_PACKAGE_VENDOR "scrcpy-gui")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "scrcpy-gui")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "scrcpy-gui")
set(CPACK_NSIS_DISPLAY_NAME "scrcpy-gui")
set(CPACK_NSIS_PACKAGE_NAME "scrcpy-gui")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};scrcpy-ui;gui;/")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
  set(SCRCPY_PACKAGE_ARCH arm64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(SCRCPY_PACKAGE_ARCH x64)
else()
  set(SCRCPY_PACKAGE_ARCH x86)
endif()
set(CPACK_PACKAGE_FILE_NAME "scrcpy-gui-windows-${SCRCPY_PACKAGE_ARCH}")
include(CPack)
