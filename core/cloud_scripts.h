#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// Cloud script library.
//
// Scripts live on the protected `verified` branch of the URUSHI repository (maintainer-reviewed only):
//   catalog.json                      index (id, version, files + sha256 ...)
//   community/<id>/<files...>         script files
// The client downloads the catalog on demand, installs a script into <gameDir>\scripts\cloud\<id>\ after
// verifying every file's SHA-256 against the catalog, and never runs anything that was not explicitly
// installed AND enabled by the user. Nothing here is fetched automatically.
namespace CloudScripts {

struct FileSpec {
    std::string path;     // relative, validated
    std::string sha256;   // lowercase hex
};

struct Item {
    std::string id, name, nameRu, author, version, description, descriptionRu, minUrushi, entry;
    std::vector<FileSpec> files;
    // filled by GetSnapshot()
    bool installed = false;
    std::string installedVersion;
    bool updateAvailable = false;
    std::string state;    // "" | "installing" | "ok" | "error"
    std::string error;
};

enum class CatalogState { Idle, Fetching, Ready, Error };

struct Snapshot {
    CatalogState state = CatalogState::Idle;
    std::string error;
    std::string ref;
    std::vector<Item> items;
};

struct Installed {
    std::string id, name, nameRu, version, entry;
};

void Init(const std::wstring& exeDir);
void Shutdown();

void RefreshAsync();                         // download + parse catalog.json
void InstallAsync(const std::string& id);    // install or update from the last catalog
bool Remove(const std::string& id, std::string& err);

Snapshot GetSnapshot();
std::vector<Installed> ListInstalled();
// Resolves <gameDir>\scripts\cloud\<id>\<entry>; false if not installed / invalid.
bool ResolveEntry(const std::string& id, std::wstring& outFile, std::wstring& outDir);

// ---- exposed for tests ----
bool ParseCatalog(const std::string& json, std::vector<Item>& out, std::string& err);
std::string Sha256Hex(const uint8_t* data, size_t size);
bool ValidId(const std::string& id);
bool ValidRelPath(const std::string& path);
bool VersionAtLeast(const std::string& have, const std::string& need);
using FetchFn = bool (*)(const std::string& repoRelPath, uint64_t maxBytes, std::vector<uint8_t>& out, DWORD& status);
void SetFetchForTests(FetchFn fn);
bool InstallBlocking(const Item& item, std::string& err);   // synchronous install (used by InstallAsync)

} // namespace CloudScripts
