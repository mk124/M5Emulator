# Copyright (C) 2026 MK124 and contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.30)
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()
project(M5Emulator LANGUAGES CXX)

if(APPLE)
    add_compile_options(-Werror=unguarded-availability-new)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2>=2.0.18)

set(M5EMU_QEMU_EXECUTABLE "${CMAKE_CURRENT_SOURCE_DIR}/.deps/qemu/build/qemu-system-xtensa"
    CACHE FILEPATH "QEMU executable for this simulator build")

add_library(emulator-ui STATIC
    src/models/Device.cpp
    src/models/stopwatch/StopWatch.cpp
    src/ui/DeviceUI.cpp
    src/ui/EmulatorControls.cpp
    src/ui/EmulatorControls.Menus.cpp
    src/ui/Display.cpp
    src/ui/Display.Text.cpp
    src/ui/PixelFont.cpp
    src/ui/SdlSession.cpp
    src/ui/Menu.cpp
    src/ui/Input.cpp
    src/ui/Input.Motion.cpp
    src/models/stopwatch/StopWatchLayout.cpp
    src/models/stopwatch/StopWatchUI.cpp
)
set_target_properties(emulator-ui PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_include_directories(emulator-ui PUBLIC src shared/include)
target_link_libraries(emulator-ui PUBLIC PkgConfig::SDL2)
target_compile_definitions(emulator-ui PUBLIC
    SDL_MAIN_HANDLED
    M5EMU_DEFAULT_QEMU="${M5EMU_QEMU_EXECUTABLE}"
    M5EMU_QEMU_BIOS="${CMAKE_CURRENT_SOURCE_DIR}/.deps/qemu/pc-bios"
)
target_compile_options(emulator-ui PRIVATE -Wall -Wextra -Wpedantic)

add_executable(m5-emulator
    src/Main.cpp

    src/app/Application.cpp
    src/app/Application.Session.cpp
    src/app/Application.Events.cpp
    src/app/Application.Capture.cpp
    src/app/Options.cpp
    src/app/RecentFiles.cpp
    src/app/Firmware.cpp
    src/app/RunFiles.cpp
    src/app/QemuProcess.cpp
    src/app/QemuProcess.Output.cpp
    src/app/QemuProcess.Recording.cpp
    src/app/OutputLog.cpp
)
set_target_properties(m5-emulator PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(m5-emulator PRIVATE src shared/include)
target_link_libraries(m5-emulator PRIVATE emulator-ui)
if(NOT APPLE)
    target_sources(m5-emulator PRIVATE src/app/VideoRecorder.Unsupported.cpp)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    target_link_libraries(m5-emulator PRIVATE OpenSSL::Crypto)
endif()
target_compile_options(m5-emulator PRIVATE -Wall -Wextra -Wpedantic)

set(resources
    resources/AppIcon.icns
    resources/fonts/unifont-15.1.05.hex
    resources/fonts/unifont-15.1.05-license.txt
    resources/fonts/README.md
    resources/esp32s3/bootloader.bin
    resources/esp32s3/LICENSE
    resources/esp32s3/README.md
)
foreach(resource IN LISTS resources)
    string(REGEX REPLACE "^resources/" "" relative_path "${resource}")
    # SDL resolves resources beside the CLI entry point or inside the app bundle.
    configure_file("${resource}" "${relative_path}" COPYONLY)
    if(APPLE)
        get_filename_component(directory "${relative_path}" DIRECTORY)
        set_source_files_properties("${resource}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/${directory}")
        target_sources(m5-emulator PRIVATE "${resource}")
    endif()
endforeach()

if(APPLE)
    enable_language(OBJCXX)
    add_library(ble-peer STATIC
        src/bluetooth/BleBytes.cpp
        src/bluetooth/SmpCrypto.cpp
        src/bluetooth/SmpSession.cpp
        src/bluetooth/VirtualPeer.cpp
        src/bluetooth/VirtualPeer.Att.cpp
        src/bluetooth/VirtualPeer.Discovery.cpp
        src/bluetooth/VirtualPeer.Catalog.cpp
        qemu/esp32s3/bluetooth/PeerSocket.cpp
    )
    set_target_properties(ble-peer PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_include_directories(ble-peer PUBLIC src qemu/esp32s3/bluetooth)
    target_sources(ble-peer PRIVATE
        src/bluetooth/macos/SmpCrypto.cpp
        src/bluetooth/macos/SmpKeyPair.cpp
    )
    target_link_libraries(ble-peer PUBLIC "-framework Security" "-framework Foundation")
    target_compile_options(ble-peer PRIVATE -Wall -Wextra -Wpedantic)
    target_sources(m5-emulator PRIVATE
        src/app/VideoRecorder.mm
        src/bluetooth/macos/BluetoothBridge.mm
        src/bluetooth/macos/PeripheralDelegate.mm
        src/bluetooth/macos/PeripheralDelegate.Requests.mm
    )
    set_target_properties(m5-emulator PROPERTIES OBJCXX_STANDARD 26 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
    target_compile_options(m5-emulator PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(m5-emulator PRIVATE ble-peer "-framework CoreBluetooth" "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo")

    file(READ "${CMAKE_ROOT}/Modules/MacOSXBundleInfo.plist.in" bundle_info)
    string(REPLACE "</dict>"
        "    <key>LSMinimumSystemVersion</key>\n    <string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>\n    <key>NSMicrophoneUsageDescription</key>\n    <string>Use the microphone as the emulated device audio input when MIC is enabled.</string>\n    <key>NSBluetoothAlwaysUsageDescription</key>\n    <string>Connect Bluetooth clients to the original firmware running in M5 Emulator when BLE is enabled.</string>\n</dict>"
        bundle_info "${bundle_info}")
    file(WRITE "${CMAKE_BINARY_DIR}/SimulatorInfo.plist.in" "${bundle_info}")
    set_target_properties(m5-emulator PROPERTIES
        MACOSX_BUNDLE YES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/SimulatorInfo.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.m5emulator"
        MACOSX_BUNDLE_BUNDLE_NAME "M5 Emulator"
        MACOSX_BUNDLE_ICON_FILE "AppIcon"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
    )
    add_custom_command(TARGET m5-emulator POST_BUILD
        COMMAND /usr/bin/codesign --force --sign - "$<TARGET_BUNDLE_DIR:m5-emulator>"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_BINARY_DIR}/m5-emulator"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "$<TARGET_FILE:m5-emulator>"
                "${CMAKE_BINARY_DIR}/m5-emulator"
    )
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(input-test tests/InputTest.cpp)
    set_target_properties(input-test PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_link_libraries(input-test PRIVATE emulator-ui)
    target_compile_options(input-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME input COMMAND input-test)

    add_executable(display-test tests/DisplayTest.cpp)
    set_target_properties(display-test PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_link_libraries(display-test PRIVATE emulator-ui)
    target_compile_options(display-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME display COMMAND display-test)

    add_executable(recent-files-test tests/RecentFilesTest.cpp src/app/RecentFiles.cpp)
    set_target_properties(recent-files-test PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_include_directories(recent-files-test PRIVATE src)
    target_link_libraries(recent-files-test PRIVATE PkgConfig::SDL2)
    target_compile_definitions(recent-files-test PRIVATE SDL_MAIN_HANDLED)
    target_compile_options(recent-files-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME recent-files COMMAND recent-files-test)
endif()
