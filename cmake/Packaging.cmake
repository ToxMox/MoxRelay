# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ToxMox / MoxRelay contributors
#
# Packaging.cmake -- CPack ZIP packaging that produces a CLEAN, PORTABLE MoxRelay/ folder + zip.
#
# ALLOWLIST BY CONSTRUCTION: every shipped file is named or pattern-matched from a SOURCE tree (the
# obs rundir, the Qt bundle, the MSVC CRT list, the repo root) -- NEVER a blind copy of the build/exe
# dir. So dev/runtime artifacts that accumulate next to the built exe (*.pdb, *.lib, *.log, *.jsonl,
# *.err, verify-*.png, a *-fleet.json dev fixture) CANNOT leak into the package:
# none of them live in any source tree these rules pull from.
#
# SINGLE SOURCE OF TRUTH: the install() rules pull from the SAME shared MOXRELAY_DEPLOY_* source vars
# that cmake/DeployRuntime.cmake's POST_BUILD copies use, so the dev build-dir deploy and the packaged
# zip can never ship different sets. This module is include()d from CMakeLists.txt RIGHT AFTER
# DeployRuntime (so those shared vars + the moxrelay/Qt6/obs targets are in scope), and AFTER the
# version vars (MOXRELAY_VERSION_FULL / MOXRELAY_VERSION_NUMERIC) are derived from HelperConfig.hpp.
#
# This module ships the BINARIES + the root LICENSE/README + the GPL/third-party compliance set
# (SOURCES.txt corresponding-source offer, THIRD-PARTY-NOTICES.txt index, and licenses/ texts).
# SOURCES.txt is GENERATED at configure time from packaging/SOURCES.txt.in (rule 6a below): the
# app-version lines are stamped from MOXRELAY_VERSION_FULL so the shipped offer can never drift
# from the built version. Only the .in template exists in the source tree -- there is no static
# SOURCES.txt to go stale.
#
# ---- Produce the zip (from a VS dev shell) ----
#   cmake --preset msvc-x64                 # configure (real paths come from CMakeUserPresets.json)
#   cmake --build --preset msvc-x64         # build moxrelay.exe (RelWithDebInfo) + run the POST_BUILD deploy
#   cpack -G ZIP -C RelWithDebInfo --config build/CPackConfig.cmake
# Output: build/dist/MoxRelay-<version>-win-x64.zip, extracting to one clean MoxRelay/ folder.

# =============================================================================================
# install() manifest (the allowlist)
# =============================================================================================

# 1. The executable -> package ROOT.
install(TARGETS moxrelay RUNTIME DESTINATION .)

# 2. Qt module DLLs (Core/Gui/Widgets) -> ROOT; the Qt platform plugin -> platforms/ (Qt resolves
#    "<exeDir>/platforms/qwindows.dll" at QApplication init). Both pull the SAME gen-expr sources as
#    DeployRuntime's POST_BUILD step 1+2.
install(FILES ${MOXRELAY_DEPLOY_QT_MODULE_DLLS} DESTINATION .)
install(FILES "${MOXRELAY_DEPLOY_QT_PLATFORM_PLUGIN}" DESTINATION platforms)

# 3. MSVC CRT (vcruntime140 / vcruntime140_1 / msvcp140 / concrt140) -> ROOT, next to the exe.
#    CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS is populated by include(InstallRequiredSystemLibraries) in the
#    top-level CMakeLists; that module's OWN install rule is suppressed there (..._SKIP=TRUE) so the
#    CRT is part of THIS explicit manifest and lands at the package ROOT (not the module default bin/).
if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
	install(FILES ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION .)
endif()

# 4. obs runtime, pulled from the rundir source trees (NOT the build dir). Guarded on the rundir like
#    DeployRuntime's POST_BUILD copies.
#      a. libobs core + FFmpeg + encoder/net DLLs from bin/64bit -> ROOT. FILES_MATCHING "*.dll" is the
#         allowlist: only DLLs ship (no *.pdb / *.lib / helper *.exe from that dir).
#      b. the data/ tree -> data/  (drop *.pdb; ALSO drop the DORMANT virtual-camera artifacts that
#         live under data/obs-plugins/win-dshow/ -- obs-virtualcam-module32.dll + obs-virtualcam-module64.dll,
#         the virtualcam-install/uninstall .bat scripts, and the virtual-camera placeholder.png. MoxRelay's
#         only live output is Spout; it never starts/registers the virtual camera, and win-dshow's
#         obs_module_load() registers that output ONLY when the COM filter is already installed in the
#         registry -- so excluding these never affects module load (Gate B stays 0 failures) nor the
#         dshow_input CAMERA INPUT MoxRelay DOES use. The matching *.pdb are dropped by the *.pdb rule.).
#      c. the obs-plugins/ tree -> obs-plugins/  (drop only *.pdb; the 32-bit capture helpers
#         graphics-hook32 / inject-helper32 / get-graphics-offsets32 are KEPT).
if(MOXRELAY_OBS_RUNDIR)
	install(DIRECTORY "${MOXRELAY_DEPLOY_OBS_BIN_DIR}/"
		DESTINATION .
		FILES_MATCHING PATTERN "*.dll")

	# data tree MINUS the per-plugin data subtree (curated below). Keeps data/libobs (+ any other
	# core data) whole; the ENTIRE data/obs-plugins/ subtree is excluded here and re-added per
	# allowlisted plugin, so dropped plugins leave NO orphaned data behind.
	install(DIRECTORY "${MOXRELAY_DEPLOY_OBS_DATA_DIR}/"
		DESTINATION data
		PATTERN "*.pdb" EXCLUDE
		PATTERN "obs-plugins" EXCLUDE)

	# per-plugin data dirs (ALLOWLIST): win-capture's graphics-hook/inject-helper/offsets payload,
	# obs-filters' .effect/LUT set, each plugin's locale/. win-dshow drops its DORMANT virtual-camera
	# artifacts -- preserving the existing virtualcam allowlist-by-construction exclusion (Gate B
	# unaffected; dshow_input camera input keeps working). The virtualcam/placeholder excludes are
	# harmless no-ops for the other six plugins.
	foreach(_plugin IN LISTS MOXRELAY_OBS_PLUGIN_ALLOWLIST)
		install(DIRECTORY "${MOXRELAY_DEPLOY_OBS_DATA_DIR}/obs-plugins/${_plugin}"
			DESTINATION data/obs-plugins
			PATTERN "*.pdb" EXCLUDE
			PATTERN "obs-virtualcam-module*.dll" EXCLUDE
			PATTERN "virtualcam-*.bat" EXCLUDE
			PATTERN "placeholder.png" EXCLUDE)
	endforeach()

	# obs plugin DLLs (ALLOWLIST). install(FILES) of named DLLs is the allowlist; replaces the old
	# whole-tree install that shipped every plugin (incl. the 6 post-load GUI crashers).
	foreach(_plugin IN LISTS MOXRELAY_OBS_PLUGIN_ALLOWLIST)
		install(FILES "${MOXRELAY_DEPLOY_OBS_PLUGINS_DIR}/64bit/${_plugin}.dll"
			DESTINATION obs-plugins/64bit)
	endforeach()
endif()

# 5. Root LICENSE + README -> ROOT.
install(FILES
	"${CMAKE_SOURCE_DIR}/LICENSE"
	"${CMAKE_SOURCE_DIR}/README.md"
	DESTINATION .)

# 6. GPL/third-party compliance set. The whole binary is GPL (libobs + a GPL FFmpeg
#    build + x264) and redistributes third-party binaries, so the package must ship
#    the corresponding-source written offer + a third-party license index + the
#    per-component license texts.
#      a. SOURCES.txt (GPL written offer): STAMPED at configure time from packaging/SOURCES.txt.in.
#         configure_file(@ONLY) substitutes @MOXRELAY_VERSION_FULL@ (the full kMoxRelayVersion
#         string incl. any -m<n> suffix, parsed from HelperConfig.hpp in CMakeLists.txt) into the
#         app-version lines, then the GENERATED file installs to the package ROOT. The pinned
#         third-party revisions in the template (obs/obs-deps/FFmpeg/x264) stay hand-maintained.
#         The template is the only copy in the source tree, so a stale unstamped SOURCES.txt
#         cannot ship. THIRD-PARTY-NOTICES.txt (license index) -> ROOT, verbatim.
#      b. the per-component verbatim license texts -> licenses/  (the trailing "/" on the
#         source copies the CONTENTS of packaging/licenses into the package's licenses/).
if(NOT MOXRELAY_VERSION_FULL)
	message(FATAL_ERROR "MOXRELAY_VERSION_FULL is not set; Packaging.cmake must be included after the version block in CMakeLists.txt")
endif()
configure_file(
	"${CMAKE_SOURCE_DIR}/packaging/SOURCES.txt.in"
	"${CMAKE_BINARY_DIR}/packaging/SOURCES.txt"
	@ONLY)

install(FILES
	"${CMAKE_BINARY_DIR}/packaging/SOURCES.txt"
	"${CMAKE_SOURCE_DIR}/packaging/THIRD-PARTY-NOTICES.txt"
	DESTINATION .)

install(DIRECTORY "${CMAKE_SOURCE_DIR}/packaging/licenses/"
	DESTINATION licenses)

# 7. User docs -> docs/. The shipped subset only (building.md is hosted-only and is NOT shipped).
#    Named explicitly (not a directory glob) so only the curated user-facing set lands in the package.
install(FILES
	"${CMAKE_SOURCE_DIR}/docs/usage.md"
	"${CMAKE_SOURCE_DIR}/docs/spout-receiving.md"
	"${CMAKE_SOURCE_DIR}/docs/control-api.md"
	"${CMAKE_SOURCE_DIR}/docs/control-api.asyncapi.yaml"
	"${CMAKE_SOURCE_DIR}/docs/cli.md"
	DESTINATION docs)

# =============================================================================================
# CPack ZIP config (package/folder/file names must stay unbranded -- "moxrelay" only)
# =============================================================================================
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "moxrelay")

# CPack requires a NUMERIC version here.
set(CPACK_PACKAGE_VERSION "${MOXRELAY_VERSION_NUMERIC}")

# Two distinct names:
#   * CPACK_PACKAGE_FILE_NAME = the staging dir name == the TOP-LEVEL FOLDER inside the zip (when
#     CPACK_INCLUDE_TOPLEVEL_DIRECTORY is ON). Keep it the clean, unversioned "MoxRelay" so the archive
#     extracts to one MoxRelay/ folder.
#   * CPACK_ARCHIVE_FILE_NAME = the OUTPUT zip file name. Version it with the FULL kMoxRelayVersion
#     (incl. any pre-release suffix) so the filename matches the version the binary reports.
set(CPACK_PACKAGE_FILE_NAME "MoxRelay")
set(CPACK_ARCHIVE_FILE_NAME "MoxRelay-${MOXRELAY_VERSION_FULL}-win-x64")

set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/dist")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)

# include(CPack) LAST -- it snapshots the CPACK_* vars above into CPackConfig.cmake.
include(CPack)
