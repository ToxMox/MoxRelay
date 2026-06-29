// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// ProfileStore implementation. Resolves the ROAMING %APPDATA%/MoxRelay/profiles path (the same
// known-folder idiom HelperConfig::canonicalConfigPath uses, FOLDERID_RoamingAppData), validates
// profile names, and atomically writes/reads/lists/deletes profile JSON files + the state.json
// last-profile pointer. Atomic writes reuse the temp-file + MoveFileEx rename pattern from
// HelperConfig (a reader never sees a torn file). state.json is serialized via libobs obs_data (no
// new dependency; obs_data is bmem-only and works with obs.dll merely loaded).

#include "ProfileStore.hpp"

#include <obs-data.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace moxrelay {

namespace {

// The ROAMING %APPDATA% base (forward-slash normalized, NO trailing slash), or "" if it cannot be
// resolved. Same idiom as HelperConfig::canonicalConfigPath (FOLDERID_RoamingAppData) -- profiles +
// state are ROAMING for symmetry with the discovery file + MoxRelay.ini; the module cache (item 02)
// is the SEPARATE %LOCALAPPDATA%/MoxRelay dir.
std::string roamingAppDataDir()
{
#ifdef _WIN32
	PWSTR appData = nullptr;
	std::string base;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData) {
		int n = WideCharToMultiByte(CP_UTF8, 0, appData, -1, nullptr, 0, nullptr, nullptr);
		if (n > 0) {
			base.resize(static_cast<size_t>(n) - 1);
			WideCharToMultiByte(CP_UTF8, 0, appData, -1, base.data(), n, nullptr, nullptr);
		}
	}
	if (appData)
		CoTaskMemFree(appData);

	for (char &c : base)
		if (c == '\\')
			c = '/';
	return base;
#else
	return std::string();
#endif
}

// Atomic write: temp file (pid-suffixed) + rename over the target. Creates the parent dir (single
// level beyond %APPDATA%/MoxRelay -- the MoxRelay segment may not exist yet either, so create both).
bool atomic_write(const std::string &path, const std::string &json)
{
#ifdef _WIN32
	if (path.empty())
		return false;

	// Create every parent segment under %APPDATA% (MoxRelay, then MoxRelay/profiles). A single
	// CreateDirectoryA only makes the leaf; walk the path so a missing MoxRelay parent is created too.
	const size_t lastSlash = path.find_last_of('/');
	if (lastSlash != std::string::npos) {
		const std::string dir = path.substr(0, lastSlash);
		size_t pos = dir.find('/', 3); // skip the drive root (e.g. "C:/")
		while (pos != std::string::npos) {
			const std::string seg = dir.substr(0, pos);
			if (!CreateDirectoryA(seg.c_str(), nullptr) &&
			    GetLastError() != ERROR_ALREADY_EXISTS) {
				fprintf(stderr, "[ProfileStore] CreateDirectory('%s') failed (err=%lu)\n",
					seg.c_str(), GetLastError());
				return false;
			}
			pos = dir.find('/', pos + 1);
		}
		if (!CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
			fprintf(stderr, "[ProfileStore] CreateDirectory('%s') failed (err=%lu)\n", dir.c_str(),
				GetLastError());
			return false;
		}
	}

	const std::string tmp = path + ".tmp-" + std::to_string(GetCurrentProcessId());
	FILE *f = nullptr;
	if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f) {
		fprintf(stderr, "[ProfileStore] cannot open temp file '%s'\n", tmp.c_str());
		return false;
	}
	const size_t written = fwrite(json.data(), 1, json.size(), f);
	const bool flushed = (fflush(f) == 0);
	fclose(f);
	if (written != json.size() || !flushed) {
		fprintf(stderr, "[ProfileStore] short write to '%s'\n", tmp.c_str());
		DeleteFileA(tmp.c_str());
		return false;
	}
	if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		fprintf(stderr, "[ProfileStore] rename '%s' -> '%s' failed (err=%lu)\n", tmp.c_str(),
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

std::string toLowerAscii(std::string s)
{
	for (char &c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

} // namespace

std::string ProfileStore::profilesDir()
{
	const std::string base = roamingAppDataDir();
	if (base.empty())
		return "MoxRelay/profiles";
	return base + "/MoxRelay/profiles";
}

std::string ProfileStore::statePath()
{
	const std::string base = roamingAppDataDir();
	if (base.empty())
		return "MoxRelay/state.json";
	return base + "/MoxRelay/state.json";
}

bool ProfileStore::isValidName(const std::string &name)
{
	if (name.empty() || name.size() > 128)
		return false;
	// Reject anything that could escape the profiles dir or trip the filesystem: path separators,
	// drive markers, the relative segments, and the Windows-reserved characters. A valid name is a
	// plain label the user types in the toolbar.
	if (name == "." || name == "..")
		return false;
	for (char c : name) {
		const unsigned char u = static_cast<unsigned char>(c);
		if (u < 0x20)
			return false; // control chars
		if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
		    c == '>' || c == '|')
			return false;
	}
	// No leading/trailing dot or space (Windows strips trailing dots/spaces from file names).
	if (name.front() == ' ' || name.back() == ' ' || name.back() == '.')
		return false;
	return true;
}

std::string ProfileStore::profilePath(const std::string &name)
{
	if (!isValidName(name))
		return std::string();
	return profilesDir() + "/" + name + ".json";
}

std::vector<std::string> ProfileStore::listProfiles()
{
	std::vector<std::string> names;
#ifdef _WIN32
	const std::string pattern = profilesDir() + "/*.json";
	WIN32_FIND_DATAA fd = {};
	HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			std::string file = fd.cFileName;
			const std::string ext = ".json";
			if (file.size() <= ext.size())
				continue;
			if (toLowerAscii(file.substr(file.size() - ext.size())) != ext)
				continue;
			const std::string bare = file.substr(0, file.size() - ext.size());
			if (isValidName(bare))
				names.push_back(bare);
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
#endif
	std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
		return toLowerAscii(a) < toLowerAscii(b);
	});
	return names;
}

bool ProfileStore::exists(const std::string &name)
{
#ifdef _WIN32
	const std::string path = profilePath(name);
	if (path.empty())
		return false;
	const DWORD attrs = GetFileAttributesA(path.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
	(void)name;
	return false;
#endif
}

bool ProfileStore::read(const std::string &name, std::string &out)
{
	const std::string path = profilePath(name);
	if (path.empty())
		return false;
	FILE *f = nullptr;
	if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
		return false;
	std::string data;
	char buf[8192];
	size_t n = 0;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		data.append(buf, n);
	fclose(f);
	out = std::move(data);
	return true;
}

bool ProfileStore::write(const std::string &name, const std::string &json)
{
	const std::string path = profilePath(name);
	if (path.empty())
		return false;
	return atomic_write(path, json);
}

bool ProfileStore::remove(const std::string &name)
{
#ifdef _WIN32
	const std::string path = profilePath(name);
	if (path.empty())
		return false;
	if (DeleteFileA(path.c_str()))
		return true;
	return GetLastError() == ERROR_FILE_NOT_FOUND; // already gone is success
#else
	(void)name;
	return false;
#endif
}

std::string ProfileStore::lastProfile()
{
	const std::string path = statePath();
	std::string result;
	if (obs_data_t *doc = obs_data_create_from_json_file(path.c_str())) {
		const char *last = obs_data_get_string(doc, "lastProfile");
		if (last)
			result = last;
		obs_data_release(doc);
	}
	return result;
}

bool ProfileStore::setLastProfile(const std::string &name)
{
	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "lastProfile", name.c_str());
	const char *json = obs_data_get_json_pretty(root);
	const std::string text = json ? json : "{}";
	obs_data_release(root);
	return atomic_write(statePath(), text);
}

} // namespace moxrelay
