#pragma once

// The public keys the launcher trusts for release manifests. A manifest is
// accepted when its signature verifies against any key listed here.
//
// Each key is a P-256 public key written as 128 hex characters: the x
// coordinate (64 characters) followed by the y coordinate (64 characters).
// `python tools\sign_manifest.py generate` makes a pair and prints both halves
// once. The public half goes in this list; the private half goes in the
// maintainer's password manager and never into a repository, a log or a
// session. The list holds the main key first and a recovery key second, so that
// losing one does not strand installed launchers.
//
// tools\sign_manifest.py reads this file and signs only with a key listed here.
// Keep each key as one string on one line, which is how the tool finds it.

inline constexpr const char* kUpdatePublicKeys[] = {
    // main key, generated 2026-11-07
    "ff221508119ee9f057ce77d73d3f56641871687b954bc4003eae8e1dffb7c6e0b1f04872754b7a0ba528ef99ee2da77588caf44105361aae2f908da1407fa48c",
    // recovery key, generated 2026-11-07
    "a6405e31073af6f8542704362456b355b8896412192c366d952514cbe5d8db150a234bcdb0195a0a26071ed3c0d7772083b2beebb4a384a4b7920af7321f0951",
    nullptr,
};
