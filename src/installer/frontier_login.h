// The login a Frontier-store direct launch remembers, in the launcher's own folder.
//
//   <launcher folder>\frontier-login.dat
//
// It holds the email in the clear, the machine token encrypted, and the password encrypted only when the
// user ticked "Remember my password". Encryption is DPAPI for the current Windows user (CryptProtectData)
// with an entropy string of this program's own, so another Windows user cannot read it and nor can another
// program that does not know the string; a program running as the same user can still ask Windows to
// decrypt it, which is what DPAPI is. Under Wine and Proton the protection is weaker still.
//
// min-ed-launcher keeps the same three things in a file of its own, encrypted with a salt taken out of
// Frontier's ClientSupport.dll. Nothing of Frontier's is read here.
//
// A file that is missing, cut short, edited, or encrypted for another user reads as no login at all. The
// file is written atomically, and nothing in it appears in a log, an error or a plan file: this header's
// functions return no text of its contents except through StoredLogin.
#pragma once

#include <string>

namespace edvr::installer {

struct StoredLogin {
    std::string email;
    std::string password;      // empty when it was not remembered
    std::string machineToken;  // empty until a verification code has been accepted on this PC
};

// The file's name inside the launcher folder.
inline constexpr const wchar_t* kFrontierLoginFile = L"frontier-login.dat";

// False when there is no usable login: the file is missing, damaged, or does not decrypt. `out` is then empty.
bool loadFrontierLogin(const std::wstring& path, StoredLogin* out);

// Writes the file, replacing any there. An empty password or machine token is left out. False when it
// cannot be written, with a short sentence in `error` that holds none of the values.
bool saveFrontierLogin(const std::wstring& path, const StoredLogin& login, std::string* error);

// Keeps the email and drops the password and the machine token.
bool forgetFrontierSecrets(const std::wstring& path);

// Deletes the file. True when it is gone afterwards, including when it was never there.
bool deleteFrontierLogin(const std::wstring& path);

// Overwrites a string's bytes before it is cleared. Best effort: copies made earlier are not reached.
void wipeString(std::string* text);

}  // namespace edvr::installer
