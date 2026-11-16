// Port settings remembered between launches (black.ini next to black.exe): what PC SETTINGS
// change. Command-line flags win over the file for that session. Test runs (--background) neither read nor
// write it.
#pragma once

#include <string>

void settings_mark_cli(const char *key); // a command-line flag set this setting: the file must not override it
void settings_cli_set(const std::string &key_value); // --set key=value: a black.ini line from the command line (tests)
void settings_load();                    // after the command line, once the root folder is current
void settings_save();
