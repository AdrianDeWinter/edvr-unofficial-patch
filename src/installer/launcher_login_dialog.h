// The two small modal windows a Frontier-store direct launch asks its questions in: the login, and the
// code Frontier emails. They are drawn like the launcher's own window (ui.h): the same colours and
// fonts, owner-drawn buttons and a drawn frame around each text box.
//
// Return signs in, Escape cancels, and a form with an empty field cannot be submitted by either. The
// password box is masked, and what was typed is cleared from the controls before the window goes.
//
// Both run their own message loop and return when the window is closed, so they are called on the
// window's thread. Compiled only into the launcher (and its rig, which drives them without showing them).
#pragma once

#include <windows.h>

#include <string>

#include "direct_launch.h"

namespace edvr::installer {

// The window class of both, so the rig can find the window it is driving.
inline constexpr const wchar_t* kFrontierDialogClass = L"EdUpFrontierDialog";

struct FrontierDialogOptions {
    HWND owner = nullptr;  // disabled while the dialog is up, and the window it is centred on
    UINT dpi = 96;
    bool underWine = false;  // the privacy line says the saved password is not strongly protected here
    bool visible = true;     // false: the rig drives a window nobody sees
};

// The login. `form` holds the email to start from and the answer on return; true is Sign in, false is Cancel.
// `note` is a line in the warning colour (why it is asked again), or empty.
bool showFrontierLogin(const FrontierDialogOptions& options, const std::string& note, LoginForm* form);

// The code. `email` is where it was sent, and is said. True with the trimmed code, false for Cancel.
bool showFrontierCode(const FrontierDialogOptions& options, const std::string& email, const std::string& note, std::string* code);

}  // namespace edvr::installer
