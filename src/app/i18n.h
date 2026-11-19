// UI string translation (English keys -> current language) and layout direction.
#pragma once
#include <string>

namespace wasd::app::i18n {

void setLanguage(const std::string& code);  // "en" or "ar"
const std::string& language();
bool rtl();
// Translated text for an English source string; returns the input when no translation exists.
const char* tr(const char* en);
// printf-style formatting of a translated template (result is logical order, not shaped).
std::string trf(const char* en, ...);

}  // namespace wasd::app::i18n
