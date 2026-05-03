#pragma once
#include <string>

namespace logger {
    enum LogType { inf, wrn, err, exc, dbg }; // info | warning | error | exception | debug
    void log(const std::string_view content, const LogType type = inf);
    void initialize(const bool& log_to_file = false, const bool& debug = false);
};