#pragma once
// The seat memory's file (seat_memory.h holds the format and the decisions). It lives with the other EDVR state that survives a game update:
// %LOCALAPPDATA%\EDVR\<leaf>-<store>\seat.txt, the installer's mirror folder (src/installer/mirror.h mirrorDirFor), resolved the way the
// graphics half resolves it for the menu's safety copies, through common/mirror_location.h. Windows only; the pure half has no file in it.
#include "seat_memory.h"
#include "../common/mirror_location.h"
#include <windows.h>
#include <cwctype>
#include <string>
#include <vector>

namespace edvr::openxr {

constexpr const wchar_t* kSeatFileName = L"seat.txt";

inline bool seatDirExists(const std::wstring& p) {
  return edvr::mirrorDirectoryExists(p);
}

// `localAppData` and `exeDir` are parameters so the rig can point it at a scratch folder. `create` false never creates anything (a read of an
// install that has no mirror folder answers empty).
inline std::wstring seatMirrorDirFor(const std::wstring& localAppData, const std::wstring& exeDir, bool create) {
  return edvr::runtimeMirrorDirFor(localAppData, exeDir, create);
}

inline std::wstring seatMirrorDir(bool create) {
  wchar_t env[MAX_PATH] = {};
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH)) return std::wstring();
  wchar_t exe[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (!n || n >= MAX_PATH) return std::wstring();
  std::wstring dir(exe, n);
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return std::wstring();
  dir.resize(slash);
  return seatMirrorDirFor(env, dir, create);
}

// Absent is not an error: an empty book and true. false only for a file that exists and cannot be read, or whose header is wrong.
inline bool seatBookLoadFile(const std::wstring& path, std::vector<SeatRecord>& book, unsigned* skipped = nullptr, bool* existed = nullptr) {
  book.clear();
  if (existed) *existed = false;
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    const DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
  }
  if (existed) *existed = true;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart <= 65536;
  std::string bytes;
  if (ok) {
    bytes.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    ok = bytes.empty() || (ReadFile(f, &bytes[0], static_cast<DWORD>(bytes.size()), &got, nullptr) && got == bytes.size());
  }
  CloseHandle(f);
  return ok && seatBookParse(bytes, book, skipped);
}

// Written beside, flushed, then moved over: a crash leaves the old file or the new one, never half of either.
inline bool seatBookSaveFile(const std::wstring& path, const std::vector<SeatRecord>& book) {
  const std::string bytes = seatBookSerialize(book);
  const std::wstring tmp = path + L".tmp";
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  bool ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr) && put == bytes.size() && FlushFileBuffers(f);
  CloseHandle(f);
  if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
  if (!ok) DeleteFileW(tmp.c_str());
  return ok;
}

}  // namespace edvr::openxr
