// The launcher's own words, in the language the top control has selected.
// English is the key. A missing translation stays English.
#pragma once

#include <string>

namespace edvr::installer {

void l10nSet(const std::string& code);
const std::string& l10nCode();

// The translation of `english` for the selected language. The pointer stays
// valid until the next l10nSet, and is `english` itself when that is what shows.
const wchar_t* tr(const wchar_t* english);

// A launcher status line. Fixed English sentences are translated whole. "ED-UP <version> is available."
// and "ED-UP <version> is ready to install." keep the version and translate the rest.
std::wstring trLine(const std::string& utf8);

}  // namespace edvr::installer
