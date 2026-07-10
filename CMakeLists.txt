# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ToxMox / MoxRelay contributors
#
# MoxRelay -- M1 build. Proves: (1) Qt6 from the obs-deps bundle (find_package); (2) libobs
# bootstrapped in-process, Qt-free, in the corrected boot order (Gate A + Gate B); and
# (Stage C) the PREVIEW SEAM -- OBSQTDisplay ported to a standalone Qt6 widget (src/ui), hosting a
# libobs obs_display_create swapchain on a native QWidget HWND with a render-thread draw callback.
# The executable is built as the WINDOWS PE subsystem (no console window for any launcher); see the
# target_link_options below. Diagnostics go to a file log sink; --selftest/--perf reattach the parent
# console so dev-shell output stays visible.

cmake_minimum_required(VERSION 3.28)

# ---------------------------------------------------------------------------------------------
# Version: SINGLE SOURCE OF TRUTH = kMoxRelayVersion in src/app/HelperConfig.hpp (the version the
# running exe publishes). Derived here at configure time so the build/package version can NEVER drift
# from what the binary reports -- do NOT hardcode a version number anywhere; bump kMoxRelayVersion and
# everything (project(), CPACK_PACKAGE_VERSION, the zip filename) follows.
#   MOXRELAY_VERSION_FULL    -- full string incl. any pre-release suffix (e.g. "X.Y.Z-mN"); used for the zip FILENAME.
#   MOXRELAY_VERSION_NUMERIC -- numeric MAJOR.MINOR.PATCH core; project()/CPack require numeric here.
# Read BEFORE project() so the numeric core can feed project(... VERSION ...).
# ---------------------------------------------------------------------------------------------
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/app/HelperConfig.hpp" _moxrelay_helperconfig)
string(REGEX MATCH "kMoxRelayVersion[^\"]*\"([^\"]+)\"" _moxrelay_version_full_match "${_moxrelay_helperconfig}")
if(NOT _moxrelay_version_full_match)
	message(FATAL_ERROR "Could not parse kMoxRelayVersion from src/app/HelperConfig.hpp")
endif()
set(MOXRELAY_VERSION_FULL "${CMAKE_MATCH_1}")
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _moxrelay_version_numeric_match "${MOXRELAY_VERSION_FULL}")
if(NOT _moxrelay_version_numeric_match)
	message(FATAL_ERROR "kMoxRelayVersion '${MOXRELAY_VERSION_FULL}' is not MAJOR.MINOR.PATCH[-suffix]")
endif()
set(MOXRELAY_VERSION_NUMERIC "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")

project(moxrelay VERSION ${MOXRELAY_VERSION_NUMERIC} LANGUAGES CXX)

# Windows resource compiler (rc.exe) -- compiles resources/icons/moxrelay.rc, which stamps the
# application icon into moxrelay.exe (Explorer/taskbar/Alt-Tab). Only the real Visual Studio build
# needs this; the Ninja clangd sidecar preset (cmake-build-clangd) is configure-only and must not
# gain a new rc.exe configure dependency, so RC stays scoped to the VS generator (the .rc source is
# added under the same guard below).
if(CMAKE_GENERATOR MATCHES "Visual Studio")
	enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)   # compile resources/resources.qrc (embedded QSS theme + app-icon PNGs)

# ---------------------------------------------------------------------------------------------
# Cache inputs (real absolute values supplied by CMakeUserPresets.json -- this tracked file must
# NOT hardcode machine paths). OBS_SRC/OBS_BUILD locate the out-of-tree libobs we link against;
# the Qt6 bundle is found via CMAKE_PREFIX_PATH (set by the preset). MOXRELAY_OBS_RUNDIR is the
# absolute runtime tree the self-test boots libobs against (compiled in for this stage only).
# ---------------------------------------------------------------------------------------------
set(OBS_SRC    "" CACHE PATH   "obs-studio source clone (@32.1.2); public libobs headers live in <OBS_SRC>/libobs")
set(OBS_BUILD  "" CACHE PATH   "obs-studio out-of-tree build dir (contains libobs/<cfg>/obs.{lib,dll} and config/obsconfig.h)")
set(OBS_CONFIG "RelWithDebInfo" CACHE STRING "obs build config subfolder (RelWithDebInfo)")
set(MOXRELAY_OBS_RUNDIR "" CACHE PATH "Absolute libobs runtime tree (rundir/<cfg>) the self-test boots against")

if(NOT OBS_SRC OR NOT OBS_BUILD)
	message(FATAL_ERROR
		"OBS_SRC and OBS_BUILD must be set (see CMakeUserPresets.json). "
		"OBS_SRC='${OBS_SRC}' OBS_BUILD='${OBS_BUILD}'")
endif()

# ---------------------------------------------------------------------------------------------
# Qt6 from the obs-deps bundle. CMAKE_PREFIX_PATH (preset) points at the bundle root so
# find_package(CONFIG) resolves <bundle>/lib/cmake/Qt6/Qt6Config.cmake.
# ---------------------------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets CONFIG)

# ---------------------------------------------------------------------------------------------
# Imported SHARED target for libobs (runtime obs.dll, import lib obs.lib). INTERFACE include dirs
# expose the public libobs headers + the generated obsconfig.h.
# ---------------------------------------------------------------------------------------------
add_library(obs SHARED IMPORTED)
set_target_properties(obs PROPERTIES
	IMPORTED_IMPLIB   "${OBS_BUILD}/libobs/${OBS_CONFIG}/obs.lib"
	IMPORTED_LOCATION "${OBS_BUILD}/libobs/${OBS_CONFIG}/obs.dll"
	INTERFACE_INCLUDE_DIRECTORIES "${OBS_SRC}/libobs;${OBS_BUILD}/config")

# ---------------------------------------------------------------------------------------------
# swresample + avutil from the obs-deps prebuilt bundle (auto-downloaded into <OBS_SRC>/.deps at
# the obs configure; the dated dirname is globbed so a bundle refresh keeps working). The audio
# engine owns a raw SwrContext for its line -> device stage -- the resampler compensation API is
# not reachable through the libobs wrapper -- so it links the FFmpeg import libs directly. The
# runtime DLLs (swresample-5, avutil-59) already ship beside obs.dll via DeployRuntime.
# ---------------------------------------------------------------------------------------------
file(GLOB _MOXRELAY_FFMPEG_ROOTS "${OBS_SRC}/.deps/obs-deps-2*-x64")
list(LENGTH _MOXRELAY_FFMPEG_ROOTS _MOXRELAY_FFMPEG_ROOT_COUNT)
if(_MOXRELAY_FFMPEG_ROOT_COUNT EQUAL 0)
	message(FATAL_ERROR
		"No obs-deps bundle found under '${OBS_SRC}/.deps' (expected obs-deps-<date>-x64; "
		"configure the obs build once so the bundle downloads).")
endif()
list(GET _MOXRELAY_FFMPEG_ROOTS 0 _MOXRELAY_FFMPEG_ROOT)

add_library(moxswresample SHARED IMPORTED)
set_target_properties(moxswresample PROPERTIES
	IMPORTED_IMPLIB   "${_MOXRELAY_FFMPEG_ROOT}/lib/swresample.lib"
	IMPORTED_LOCATION "${_MOXRELAY_FFMPEG_ROOT}/bin/swresample-5.dll"
	INTERFACE_INCLUDE_DIRECTORIES "${_MOXRELAY_FFMPEG_ROOT}/include")

add_library(moxavutil SHARED IMPORTED)
set_target_properties(moxavutil PROPERTIES
	IMPORTED_IMPLIB   "${_MOXRELAY_FFMPEG_ROOT}/lib/avutil.lib"
	IMPORTED_LOCATION "${_MOXRELAY_FFMPEG_ROOT}/bin/avutil-59.dll"
	INTERFACE_INCLUDE_DIRECTORIES "${_MOXRELAY_FFMPEG_ROOT}/include")

# ---------------------------------------------------------------------------------------------
# moxspout -- the vendored Spout2 SDK (SpoutDX subset, third_party/Spout2, tag 2.007.017; see
# third_party/Spout2/VENDOR.md) as a static lib. Runtime is the CMake default /MD
# (MultiThreadedDLL) to MATCH Qt6 + libobs -- the /MT the standalone probes use does NOT transfer
# here. With neither SPOUT_BUILD_DLL nor SPOUT_IMPORT_DLL defined, SPOUT_DLLEXP collapses to
# nothing and the sources link in as plain symbols. Vendor sources keep their own warning profile.
# ---------------------------------------------------------------------------------------------
add_library(moxspout STATIC
	third_party/Spout2/SpoutDirectX/SpoutDX/SpoutDX.cpp
	third_party/Spout2/SpoutDirectX/SpoutDX/SpoutDX.h
	third_party/Spout2/SpoutGL/SpoutCommon.h
	third_party/Spout2/SpoutGL/SpoutCopy.cpp
	third_party/Spout2/SpoutGL/SpoutCopy.h
	third_party/Spout2/SpoutGL/SpoutDirectX.cpp
	third_party/Spout2/SpoutGL/SpoutDirectX.h
	third_party/Spout2/SpoutGL/SpoutFrameCount.cpp
	third_party/Spout2/SpoutGL/SpoutFrameCount.h
	third_party/Spout2/SpoutGL/SpoutSenderNames.cpp
	third_party/Spout2/SpoutGL/SpoutSenderNames.h
	third_party/Spout2/SpoutGL/SpoutSharedMemory.cpp
	third_party/Spout2/SpoutGL/SpoutSharedMemory.h
	third_party/Spout2/SpoutGL/SpoutUtils.cpp
	third_party/Spout2/SpoutGL/SpoutUtils.h)

target_include_directories(moxspout PUBLIC
	third_party/Spout2/SpoutGL
	third_party/Spout2/SpoutDirectX/SpoutDX)

if(MSVC)
	# Vendor code: keep its build tolerant (matches the probe recipe); never our own warning bar.
	target_compile_options(moxspout PRIVATE /W1 /wd4996)
endif()

target_link_libraries(moxspout PUBLIC d3d11 dxgi)

# ---------------------------------------------------------------------------------------------
# moxws -- the vendored IXWebSocket (third_party/IXWebSocket, tag v11.4.6; see its VENDOR.md) as
# a static lib for the M6 control endpoint. Loopback-only control surface: TLS and zlib stay OFF
# (no IXWEBSOCKET_USE_* defines, TLS backends not vendored), so no OpenSSL/mbedTLS/zlib enters
# the build. Sources are the upstream non-TLS list; headers live beside them in the same dir.
# ---------------------------------------------------------------------------------------------
add_library(moxws STATIC
	third_party/IXWebSocket/ixwebsocket/IXBench.cpp
	third_party/IXWebSocket/ixwebsocket/IXCancellationRequest.cpp
	third_party/IXWebSocket/ixwebsocket/IXConnectionState.cpp
	third_party/IXWebSocket/ixwebsocket/IXDNSLookup.cpp
	third_party/IXWebSocket/ixwebsocket/IXExponentialBackoff.cpp
	third_party/IXWebSocket/ixwebsocket/IXGetFreePort.cpp
	third_party/IXWebSocket/ixwebsocket/IXGzipCodec.cpp
	third_party/IXWebSocket/ixwebsocket/IXHttp.cpp
	third_party/IXWebSocket/ixwebsocket/IXHttpClient.cpp
	third_party/IXWebSocket/ixwebsocket/IXHttpServer.cpp
	third_party/IXWebSocket/ixwebsocket/IXNetSystem.cpp
	third_party/IXWebSocket/ixwebsocket/IXSelectInterrupt.cpp
	third_party/IXWebSocket/ixwebsocket/IXSelectInterruptFactory.cpp
	third_party/IXWebSocket/ixwebsocket/IXSelectInterruptPipe.cpp
	third_party/IXWebSocket/ixwebsocket/IXSelectInterruptEvent.cpp
	third_party/IXWebSocket/ixwebsocket/IXSetThreadName.cpp
	third_party/IXWebSocket/ixwebsocket/IXSocket.cpp
	third_party/IXWebSocket/ixwebsocket/IXSocketConnect.cpp
	third_party/IXWebSocket/ixwebsocket/IXSocketFactory.cpp
	third_party/IXWebSocket/ixwebsocket/IXSocketServer.cpp
	third_party/IXWebSocket/ixwebsocket/IXSocketTLSOptions.cpp
	third_party/IXWebSocket/ixwebsocket/IXStrCaseCompare.cpp
	third_party/IXWebSocket/ixwebsocket/IXUdpSocket.cpp
	third_party/IXWebSocket/ixwebsocket/IXUrlParser.cpp
	third_party/IXWebSocket/ixwebsocket/IXUuid.cpp
	third_party/IXWebSocket/ixwebsocket/IXUserAgent.cpp
	third_party/IXWebSocket/ixwebsocket/IXWebSocket.cpp
	third_party/IXWebSocket/ixwebsocket/IXWebSocketCloseConstants.cpp
	third_party/IXWebSocket/ixwebsocket/IXWebSocketHandshake.cpp
	third_party/IXWebSocket/ixwebsocket/IXWebSocketHttpHeaders.cpp
	third_party/IXWebSocket/ixwebsocket/IXWebSocketPerMessageDeflate.cpp
	third_party/IXWebSocket/ixwebsocket/IXWebSocketPerMessageDeflateCodec.cpp
	third_party/IXWebSocket/ixwebsocket/IXWebSocketPerMessageDeflateOptions.cpp
	third_party/IXWebSocket/ixwebsocket/IXWebSocketProxyServer.cpp
	third_party/IXWebSocket/ixwebsocket/IXWebSocketServer.cpp
	third_party/IXWebSocket/ixwebsocket/IXWebSocketTransport.cpp)

target_include_directories(moxws PUBLIC third_party/IXWebSocket)

if(MSVC)
	# Vendor code: keep its build tolerant (same policy as moxspout); never our own warning bar.
	target_compile_options(moxws PRIVATE /W1)
	target_compile_definitions(moxws PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

target_link_libraries(moxws PUBLIC wsock32 ws2_32 shlwapi)

# ---------------------------------------------------------------------------------------------
# WINDOWS-subsystem executable (no console window); see target_link_options below. --selftest/--perf
# reattach the parent console via LogSink; GUI mode writes to a file log sink.
# ---------------------------------------------------------------------------------------------
add_executable(moxrelay
	src/main.cpp
	src/app/AppSettings.cpp
	src/app/AppSettings.hpp
	src/app/CliOptions.cpp
	src/app/CliOptions.hpp
	src/app/LogSink.cpp
	src/app/LogSink.hpp
	src/app/ObsBootstrap.cpp
	src/app/ObsBootstrap.hpp
	src/app/ProfileStore.cpp
	src/app/ProfileStore.hpp
	src/app/SpoutNaming.cpp
	src/app/SpoutNaming.hpp
	src/app/HelperConfig.cpp
	src/app/HelperConfig.hpp
	src/app/ParentWatch.cpp
	src/app/ParentWatch.hpp
	src/app/DeviceArrivalWatch.cpp
	src/app/DeviceArrivalWatch.hpp
	src/app/RendezvousPipe.cpp
	src/app/RendezvousPipe.hpp
	src/audio/AudioDeviceNotify.cpp
	src/audio/AudioDeviceNotify.hpp
	src/audio/AudioMixEngine.cpp
	src/audio/AudioMixEngine.hpp
	src/audio/AudioRing.hpp
	src/audio/ServoTrimLogic.hpp
	src/audio/ServoTrimSim.cpp
	src/audio/ServoTrimSim.hpp
	src/control/AudioDevices.cpp
	src/control/AudioDevices.hpp
	src/control/ControlServer.cpp
	src/control/ControlServer.hpp
	src/control/ControlVerbs.cpp
	src/control/ControlVerbs.hpp
	src/control/TypeVocabulary.cpp
	src/control/TypeVocabulary.hpp
	src/obs/SourceFactory.cpp
	src/obs/SourceFactory.hpp
	src/perf/PerfHarness.cpp
	src/perf/PerfHarness.hpp
	src/spout/SenderNameAllocator.cpp
	src/spout/SenderNameAllocator.hpp
	src/spout/SpoutSenderEngine.cpp
	src/spout/SpoutSenderEngine.hpp
	src/ui/MoxDisplayWidget.cpp
	src/ui/MoxDisplayWidget.hpp
	src/ui/PreviewConsumeEngine.cpp
	src/ui/PreviewConsumeEngine.hpp
	src/ui/AudioMixerStrip.cpp
	src/ui/AudioMixerStrip.hpp
	src/ui/FilterInspector.cpp
	src/ui/FilterInspector.hpp
	src/ui/MainWindow.cpp
	src/ui/MainWindow.hpp
	src/ui/PropertyPanel.cpp
	src/ui/PropertyPanel.hpp
	src/ui/SettingsDialog.cpp
	src/ui/SettingsDialog.hpp
	src/ui/SliderJumpStyle.hpp
	src/ui/TrayController.cpp
	src/ui/TrayController.hpp
	src/ui/WheelGuard.hpp
	resources/resources.qrc)

# Q_OBJECT classes (MoxDisplayWidget, MainWindow, PropertyPanel, FilterInspector, ControlServer)
# live under src/;
# AUTOMOC handles moc generation. Headers are #include'd via "ui/..." / "app/..." / "control/..."
# from src/, so expose src/ as an include root; third_party/ provides <nlohmann/json.hpp>.
target_include_directories(moxrelay PRIVATE src third_party)

# Win32 application-icon resource -> moxrelay.exe's own icon (Explorer/taskbar/Alt-Tab), distinct
# from the runtime QApplication::setWindowIcon. Scoped to the VS generator for the reason above:
# the configure-only Ninja clangd sidecar skips RC and so must not see a .rc it cannot compile.
#
# Win32 VERSIONINFO resource -> moxrelay.exe's Properties -> Details metadata (ProductName /
# CompanyName / FileVersion / Copyright). configure_file substitutes the @VAR@ placeholders in
# moxrelay-version.rc.in from the version SSOT derived above (kMoxRelayVersion), so the stamped
# metadata can never drift from what the binary reports. The generated .rc lands in the build
# tree and is added ONLY here, inside the SAME VS-generator guard as the icon .rc (the Ninja
# clangd sidecar skips RC and must not see it).
if(CMAKE_GENERATOR MATCHES "Visual Studio")
	target_sources(moxrelay PRIVATE resources/icons/moxrelay.rc)
	configure_file(resources/icons/moxrelay-version.rc.in
		${CMAKE_CURRENT_BINARY_DIR}/moxrelay-version.rc @ONLY)
	target_sources(moxrelay PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/moxrelay-version.rc)
endif()

target_compile_definitions(moxrelay PRIVATE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
	HAVE_OBSCONFIG_H)

target_link_libraries(moxrelay PRIVATE
	Qt6::Core
	Qt6::Gui
	Qt6::Widgets
	obs
	moxswresample
	moxavutil
	moxspout
	moxws
	d3d11
	dxgi
	ole32
	avrt
	bcrypt
	cfgmgr32)

# Build as the WINDOWS subsystem so no console window appears for ANY launcher (standalone or the
# managed child launch). Keep the existing int main(argc,argv): /ENTRY:mainCRTStartup runs the CRT
# startup (full arg parsing + CRT init, then calls main()) while suppressing the console the CONSOLE
# subsystem would otherwise allocate. Diagnostics go to the file log sink (see LogSink / main.cpp).
target_link_options(moxrelay PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)

# Embed only the PDB BASENAME (moxrelay.pdb) in the exe's debug directory, not the absolute build
# path. %_PDB% is the MSVC linker macro for the PDB file name with no directory, so no machine path
# (and thus no clone-folder name) gets baked into the shipped exe. The .pdb is still produced.
target_link_options(moxrelay PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/PDBALTPATH:%_PDB%>")

# ---------------------------------------------------------------------------------------------
# Standalone deploy: POST_BUILD steps copy the runtime DLLs (Qt Core/Gui/Widgets + platform plugin,
# obs.dll + load-time deps, the obs data/ + obs-plugins/ trees, and the MSVC CRT) next to the exe so
# it launches with NO PATH setup on a CLEAN PC. Included here -- after the `moxrelay` and imported
# `obs` targets exist. See cmake/DeployRuntime.cmake.
#
# InstallRequiredSystemLibraries populates CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS with the active toolset's
# MSVC CRT DLLs (vcruntime140 / vcruntime140_1 / msvcp140 / concrt140). It MUST run before the
# DeployRuntime include so that variable is set when DeployRuntime's POST_BUILD CRT-copy step reads it.
#
# CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP=TRUE suppresses the module's OWN auto-generated install()
# rule (which would target the default bin/ subdir). The list is STILL populated regardless of this
# flag, so the POST_BUILD CRT copy is unaffected; cmake/Packaging.cmake instead installs the CRT
# EXPLICITLY to the package ROOT (DESTINATION .) as part of its allowlist manifest, matching the
# deployed exe-adjacent layout.
# ---------------------------------------------------------------------------------------------
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployRuntime.cmake)

# CPack ZIP packaging -- explicit install() allowlist that produces a CLEAN portable MoxRelay/ folder.
# Included RIGHT AFTER DeployRuntime so the shared MOXRELAY_DEPLOY_* source vars (set there) and the
# moxrelay / Qt6 / obs targets are all in scope. See cmake/Packaging.cmake.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Packaging.cmake)
