// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// HelperConfig implementation. Serializes the bare single-instance discovery object via libobs
// obs_data (no new dependency) and derives the canonical %APPDATA%/MoxRelay path. See
// HelperConfig.hpp.

#include "HelperConfig.hpp"

#include <obs-data.h>

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace moxrelay {

std::string HelperConfig::serialize(const HelperInstance &inst, bool pretty)
{
	obs_data_t *root = obs_data_create();

	obs_data_set_string(root, "instanceId", inst.instanceId.c_str());
	obs_data_set_int(root, "port", inst.port);
	obs_data_set_string(root, "version", inst.version.c_str());
	obs_data_set_int(root, "fpsTier", inst.fpsTier);
	obs_data_set_string(root, "spoutPrefix", inst.spoutPrefix.c_str());
	obs_data_set_string(root, "ownerId", inst.ownerId.c_str());
	obs_data_set_string(root, "controlToken", inst.controlToken.c_str());

	// sources[] -- libobs stores a string array as an array of {value: "..."} objects, so model
	// each source name as its own obs_data object with a single "name" field.
	obs_data_array_t *sources = obs_data_array_create();
	for (const std::string &name : inst.sources) {
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "name", name.c_str());
		obs_data_array_push_back(sources, s);
		obs_data_release(s);
	}
	obs_data_set_array(root, "sources", sources);
	obs_data_array_release(sources);

	const char *json = pretty ? obs_data_get_json_pretty(root) : obs_data_get_json(root);
	std::string result = json ? json : "{}";

	obs_data_release(root);
	return result;
}

std::string HelperConfig::serializeEmpty(bool pretty)
{
	obs_data_t *root = obs_data_create();
	const char *json = pretty ? obs_data_get_json_pretty(root) : obs_data_get_json(root);
	std::string result = json ? json : "{}";
	obs_data_release(root);
	return result;
}

bool HelperConfig::write(const HelperInstance &inst)
{
	return writeJsonTo(canonicalConfigPath(), serialize(inst, /*pretty=*/true));
}

bool HelperConfig::writeTo(const std::string &path, const HelperInstance &inst)
{
	return writeJsonTo(path, serialize(inst, /*pretty=*/true));
}

bool HelperConfig::writeEmpty()
{
	return writeJsonTo(canonicalConfigPath(), serializeEmpty(/*pretty=*/true));
}

bool HelperConfig::writeEmptyTo(const std::string &path)
{
	return writeJsonTo(path, serializeEmpty(/*pretty=*/true));
}

bool HelperConfig::writeJsonTo(const std::string &path, const std::string &json)
{
#ifdef _WIN32
	if (path.empty())
		return false;

	// Ensure the parent directory exists (single level: %APPDATA% itself always exists).
	const size_t slash = path.find_last_of('/');
	if (slash != std::string::npos) {
		const std::string dir = path.substr(0, slash);
		if (!CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
			fprintf(stderr, "[HelperConfig] CreateDirectory('%s') failed (err=%lu)\n", dir.c_str(),
				GetLastError());
			return false;
		}
	}

	const std::string tmp = path + ".tmp-" + std::to_string(GetCurrentProcessId());

	FILE *f = nullptr;
	if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f) {
		fprintf(stderr, "[HelperConfig] cannot open temp file '%s'\n", tmp.c_str());
		return false;
	}
	const size_t written = fwrite(json.data(), 1, json.size(), f);
	const bool flushed = (fflush(f) == 0);
	fclose(f);
	if (written != json.size() || !flushed) {
		fprintf(stderr, "[HelperConfig] short write to '%s'\n", tmp.c_str());
		DeleteFileA(tmp.c_str());
		return false;
	}

	// Atomic publish: rename over the target (same volume). Readers either see the old file or the
	// complete new one, never a torn intermediate.
	if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		fprintf(stderr, "[HelperConfig] rename '%s' -> '%s' failed (err=%lu)\n", tmp.c_str(),
			path.c_str(), GetLastError());
		DeleteFileA(tmp.c_str());
		return false;
	}
	return true;
#else
	(void)path;
	(void)json;
	return false;
#endif
}

std::string HelperConfig::canonicalConfigPath()
{
#ifdef _WIN32
	PWSTR appData = nullptr;
	std::string base;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData) {
		// Narrow the wide %APPDATA% path (ASCII for typical install paths; lossy only for exotic
		// non-ASCII profile dirs, acceptable for this M1 seam).
		int n = WideCharToMultiByte(CP_UTF8, 0, appData, -1, nullptr, 0, nullptr, nullptr);
		if (n > 0) {
			base.resize(static_cast<size_t>(n) - 1);
			WideCharToMultiByte(CP_UTF8, 0, appData, -1, base.data(), n, nullptr, nullptr);
		}
	}
	if (appData)
		CoTaskMemFree(appData);

	// Normalize backslashes to forward slashes for a stable cross-tool path string.
	for (char &c : base)
		if (c == '\\')
			c = '/';

	if (base.empty())
		return "MoxRelay/helper-config.json";
	return base + "/MoxRelay/helper-config.json";
#else
	return "MoxRelay/helper-config.json";
#endif
}

} // namespace moxrelay
