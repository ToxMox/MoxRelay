# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ToxMox / MoxRelay contributors
#
# DeployRuntime.cmake -- POST_BUILD deploy so the built moxrelay.exe runs STANDALONE
# (double-click / clean PATH) with NO PATH setup. Copies the runtime DLLs the loader needs
# next to the exe. This is a DEV-output deploy into the git-ignored build/ tree; size is not
# a concern. Included from CMakeLists.txt AFTER the `moxrelay` target + the imported `obs`
# target exist. Expects MOXRELAY_OBS_RUNDIR (set by the local preset) for the obs-deps copies.
#
# What gets copied into $<TARGET_FILE_DIR:moxrelay>:
#   1. Qt module DLLs  : Qt6Core.dll, Qt6Gui.dll, Qt6Widgets.dll (real paths via gen-expr).
#   2. Qt platform plug: platforms/qwindows.dll (Qt looks for "<exeDir>/platforms/qwindows.dll").
#   3. obs runtime DLLs: EVERY *.dll from ${MOXRELAY_OBS_RUNDIR}/bin/64bit -- covers obs.dll plus
#      its load-time deps (FFmpeg avcodec/avformat/avutil/swscale/swresample, zlib, w32-pthreads,
#      and anything else obs.dll imports at load).
#   4. obs data tree   : ${MOXRELAY_OBS_RUNDIR}/data -> <exeDir>/data (data/libobs/default.effect +
#      the rest). This is what makes ObsBootstrap::resolveRundir's EXE-RELATIVE branch win: it probes
#      "<exeDir>/data/libobs", so once this exists a COPIED folder boots on a clean PC. Note that
#      MOXRELAY_OBS_RUNDIR here is purely a BUILD-TIME cache var DeployRuntime reads to locate the
#      obs-deps source tree for these copies; there is NO compiled-in runtime fallback in the exe
#      anymore -- rundir resolves only via --rundir or this exe-relative data/ tree.
#   5. obs plugin tree : ${MOXRELAY_OBS_RUNDIR}/obs-plugins -> <exeDir>/obs-plugins (plugin DLLs); the
#      exe-relative rundir resolves plugin bin to <exeDir>/obs-plugins/64bit and plugin data to
#      <exeDir>/data/obs-plugins (the latter shipped by step 4).
#   6. MSVC runtime    : CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS (vcruntime140 / vcruntime140_1 / msvcp140 /
#      concrt140) so a clean PC with no VC++ redist still loads the exe + the /MD Qt6 and obs DLLs.

# ---------------------------------------------------------------------------------------------
# SHARED DEPLOY SOURCE VARS -- the SINGLE SOURCE OF TRUTH for the runtime file set. Set ONCE here and
# consumed by BOTH the POST_BUILD copies below (dev build-dir deploy) AND the install()/CPack rules in
# cmake/Packaging.cmake (the packaged zip). Packaging.cmake is include()d right after this module, so
# these are plain variables visible there (include() does not create a new scope). Changing a source
# path here updates the build-dir deploy and the packaged zip together -- the two can never drift.
#
# Source layout reminders:
#   * Qt bundle: bin/Qt6Core.dll alongside ../plugins/platforms/qwindows.dll (gen-exprs keep this free
#     of any hardcoded machine path).
#   * obs rundir (MOXRELAY_OBS_RUNDIR, from the preset): bin/64bit/*.dll, data/, obs-plugins/.
#   * MSVC CRT: CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS (full-path list from include(InstallRequiredSystemLibraries)
#     in the top-level CMakeLists) -- already a shared var, consumed verbatim by both consumers.
# ---------------------------------------------------------------------------------------------
set(MOXRELAY_DEPLOY_QT_MODULE_DLLS
	"$<TARGET_FILE:Qt6::Core>"
	"$<TARGET_FILE:Qt6::Gui>"
	"$<TARGET_FILE:Qt6::Widgets>")
set(MOXRELAY_DEPLOY_QT_PLATFORM_PLUGIN
	"$<TARGET_FILE_DIR:Qt6::Core>/../plugins/platforms/qwindows.dll")
set(MOXRELAY_DEPLOY_OBS_BIN_DIR     "${MOXRELAY_OBS_RUNDIR}/bin/64bit")
set(MOXRELAY_DEPLOY_OBS_DATA_DIR    "${MOXRELAY_OBS_RUNDIR}/data")
set(MOXRELAY_DEPLOY_OBS_PLUGINS_DIR "${MOXRELAY_OBS_RUNDIR}/obs-plugins")

# obs plugin ALLOWLIST -- the ONLY plugin modules MoxRelay ships AND loads. ObsBootstrap's
# obs_load_all_modules2 enumerates EVERY dll on the module path with NO code-level filter, so
# curation is enforced purely by shipping only these names. Consumed by BOTH the POST_BUILD
# plugin copy below AND Packaging.cmake's install() rules (shared include() scope). Covers source
# types camera/display/window/game/media/image/color/text/audio-in/audio-out + all obs-filters;
# the Spout output path needs NO plugin (libobs core). Excludes the 6 frontend GUI plugins that
# crash on post-load (frontend-tools, decklink-output-ui, decklink-captions, aja-output-ui,
# obs-websocket, obs-webrtc) plus the other unused modules.
set(MOXRELAY_OBS_PLUGIN_ALLOWLIST
	image-source
	obs-ffmpeg
	obs-filters
	obs-text
	win-capture
	win-dshow
	win-wasapi)

# ---- 1. Qt module DLLs -- copy the real Qt6Core/Gui/Widgets DLLs next to the exe. ----
add_custom_command(TARGET moxrelay POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${MOXRELAY_DEPLOY_QT_MODULE_DLLS}
		"$<TARGET_FILE_DIR:moxrelay>"
	COMMENT "[deploy] Qt module DLLs (Core/Gui/Widgets) -> exe dir")

# ---- 2. Qt platform plugin -- Qt resolves "<exeDir>/platforms/qwindows.dll" at QApplication init. ----
add_custom_command(TARGET moxrelay POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory
		"$<TARGET_FILE_DIR:moxrelay>/platforms"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${MOXRELAY_DEPLOY_QT_PLATFORM_PLUGIN}"
		"$<TARGET_FILE_DIR:moxrelay>/platforms/qwindows.dll"
	COMMENT "[deploy] Qt platform plugin -> exe dir/platforms/qwindows.dll")

# ---- 3. obs.dll + ALL its load-time deps -- copy the whole rundir/bin/64bit DLL set. ----
# copy_directory is the simplest + reliable way to pull obs.dll and every sibling load-time
# dependency in one step. Guarded on MOXRELAY_OBS_RUNDIR (always set by the local preset).
if(MOXRELAY_OBS_RUNDIR)
	add_custom_command(TARGET moxrelay POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"${MOXRELAY_DEPLOY_OBS_BIN_DIR}"
			"$<TARGET_FILE_DIR:moxrelay>"
		COMMENT "[deploy] obs.dll + load-time deps (FFmpeg/zlib/...) from rundir/bin/64bit -> exe dir")

	# ---- 4. obs DATA tree -> <exeDir>/data. resolveRundir() probes "<exeDir>/data/libobs"; once this
	# exists the EXE-RELATIVE rundir branch wins and the compiled-in dev path is never consulted on a
	# clean PC. copy_directory_if_different (CMake >= 3.26; project requires 3.28) skips unchanged files
	# so the large data tree is not recopied every build. ----
	add_custom_command(TARGET moxrelay POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			"${MOXRELAY_DEPLOY_OBS_DATA_DIR}"
			"$<TARGET_FILE_DIR:moxrelay>/data"
		COMMENT "[deploy] obs data tree (data/libobs/default.effect + data/obs-plugins) -> exe dir/data")

	# ---- 5. obs PLUGIN DLLs -> <exeDir>/obs-plugins/64bit. ALLOWLIST ONLY: the loader loads every
	# dll on the module path, so per-file copy of MOXRELAY_OBS_PLUGIN_ALLOWLIST replaces the old
	# whole-tree copy that pulled in frontend-tools/decklink/aja/websocket/webrtc (post-load GUI
	# crashers) + dead weight. Plugin DATA dirs ride along in step 4's data copy. ----
	add_custom_command(TARGET moxrelay POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory
			"$<TARGET_FILE_DIR:moxrelay>/obs-plugins/64bit"
		COMMENT "[deploy] ensure obs-plugins/64bit exists for the plugin allowlist")
	foreach(_plugin IN LISTS MOXRELAY_OBS_PLUGIN_ALLOWLIST)
		add_custom_command(TARGET moxrelay POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"${MOXRELAY_DEPLOY_OBS_PLUGINS_DIR}/64bit/${_plugin}.dll"
				"$<TARGET_FILE_DIR:moxrelay>/obs-plugins/64bit/${_plugin}.dll"
			COMMENT "[deploy] obs plugin DLL ${_plugin}.dll -> exe dir/obs-plugins/64bit")
	endforeach()
else()
	message(WARNING
		"[deploy] MOXRELAY_OBS_RUNDIR not set -- skipping obs.dll/data/plugin deploy; "
		"the built exe will NOT run standalone (obs.dll + deps must be on PATH, and the data/ + "
		"obs-plugins/ trees must reach the exe dir some other way).")
endif()

# ---- 6. MSVC runtime DLLs -- bundle the CRT so a clean PC with NO VC++ redist still loads the exe
# and the /MD Qt6 + obs DLLs (a missing CRT is a loader-level failure). CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS
# is the full-path list populated by include(InstallRequiredSystemLibraries) in the top-level
# CMakeLists (run BEFORE this module so the variable is set when this POST_BUILD step reads it). ----
if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
	add_custom_command(TARGET moxrelay POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
			"$<TARGET_FILE_DIR:moxrelay>"
		COMMENT "[deploy] MSVC runtime DLLs (vcruntime/msvcp/concrt) -> exe dir")
else()
	message(WARNING
		"[deploy] CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS is empty -- did include(InstallRequiredSystemLibraries) "
		"run before DeployRuntime in the top-level CMakeLists? The bundled VC++ runtime DLLs will be "
		"MISSING, so a clean PC without the VC++ redist will fail to load the exe.")
endif()
