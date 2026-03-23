#pragma once
#include <string>
class logger {
public:
    enum LogType { inf, wrn, err, exc, dbg }; // info | warning | error | exception | debug
    static void log(const std::string content, LogType type = inf);
    static void initialize(const bool& log_to_file = false, const bool& debug = false);
private:
    logger();
};