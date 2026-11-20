// The machine ID the Frontier servers are told: a port of MachineId.fs from min-ed-launcher
// (https://github.com/rfvgyhn/min-ed-launcher, commit 139e4ff7ab9027925648fb27767cb87acdcf290a,
// MIT licence, "Copyright (c) 2017 Chris"; the notice is third_party\min_ed_launcher\LICENSE).
//
// It is the first 16 characters, lower case, of the SHA-1 (as hex) of two registry values joined
// with nothing between them, each trimmed:
//
//   HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid
//   HKCU\SOFTWARE\Frontier Developments\Cryptography\MachineGuid
//
// The second is Frontier's own and is made, holding a random GUID, when it is missing. The ID names
// no person and no hardware beyond what Windows already calls this install, and it never leaves the
// machine except in the calls to api.zaonce.net.
//
// Where the two values live is a parameter, so tools\launcher_test can use a subkey it makes and
// removes. Both are read from the 64-bit registry view, as a 64-bit .NET process reads them.
#pragma once

#include <string>

#include <windows.h>

namespace edvr::installer {

struct MachineIdKeys {
    HKEY machineRoot = HKEY_LOCAL_MACHINE;
    std::wstring machinePath = L"SOFTWARE\\Microsoft\\Cryptography";
    HKEY frontierRoot = HKEY_CURRENT_USER;
    std::wstring frontierPath = L"SOFTWARE\\Frontier Developments\\Cryptography";
    std::wstring valueName = L"MachineGuid";
};

// The pure part: trim both, join, SHA-1, hex, first 16 characters, lower case. Empty when the
// hash cannot be computed.
std::string makeMachineId(const std::string& machineGuid, const std::string& frontierGuid);

// Reads the two values, first making the Frontier one when it is missing, and returns the ID.
// `error` says what failed, in a sentence without any value.
bool readMachineId(const MachineIdKeys& keys, std::string* id, std::string* error);

}  // namespace edvr::installer
