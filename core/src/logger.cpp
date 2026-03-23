#include "logger.h"
#include "core.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <time.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <limits.h>
  #include <shlobj.h>
#endif

static std::filesystem::path logpath;
static bool DEBUG;

const static std::filesystem::path getpath() {
#ifdef _WIN32
    PWSTR knownPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &knownPath)) && knownPath) {
        int len = WideCharToMultiByte(CP_UTF8, 0, knownPath, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8; utf8.resize(len ? len-1 : 0);
        if (len) WideCharToMultiByte(CP_UTF8, 0, knownPath, -1, &utf8[0], len, nullptr, nullptr);
        CoTaskMemFree(knownPath);
        std::filesystem::path p(utf8);
        p /= "konacode"/konacore::project/"konata-dancer-remake.log";
        return p;
    }
    if (const char* up = std::getenv("USERPROFILE")) {
        std::filesystem::path p(up);
        p /= "AppData/Local/konacode"/konacore::project/"konata-dancer-remake.log";
        return p;
    }
#endif
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".config/konacode"/konacore::project/"konata-dancer-remake.log";
    }
    return std::filesystem::path("konata-dancer-remake.log");
}

void logger::initialize(const bool& log_to_file, const bool& debug) {
    if (log_to_file) {
        logpath = getpath();
        std::error_code ec;
        if (!logpath.parent_path().empty())
            std::filesystem::create_directories(logpath.parent_path(), ec);
        if (!std::filesystem::exists(logpath)) {
            std::ofstream f(logpath);
            f.close();
            printf("\033[1m[LOGGER]\033[0m Created missing file \"%s\".\n", logpath.parent_path().c_str());
        }
        printf("\033[1m[LOGGER]\033[0m Logging to \"%s\".\n", logpath.parent_path().c_str());
    } else { printf("\033[1m[LOGGER]\033[0m Logging has been disabled!\n"); }
    if (debug) {
        printf("\033[1m[LOGGER]\033[95;1m Debug mode has been enable! You will see additional in-detail logs.\033[0m\n");
        DEBUG = std::move(debug);
    }
}


void logger::log(std::string content, LogType type) {
    time_t tm=time(0);
    struct tm * t = localtime(&tm);
    if (std::filesystem::exists(logpath)) {
        static std::ofstream f;
        f.open(logpath,std::ios_base::app);
        switch (type) {
            case inf:
                printf("\033[0;1m[INF]\033[0;90m (%.*s)\033[0m %s\n",(int)strcspn(({char *p=ctime_r(&tm,(char[26]){}); p;}),"\n"),ctime_r(&tm, (char[26]){}),content.c_str());
                f << "[INF] (" << std::put_time(t, "%c") << ") " << content << std::endl;
                break;
            case wrn:
                printf("\033[33;1m[WRN]\033[0;33m (%.*s) %s\033[0m\n",(int)strcspn(({char *p=ctime_r(&tm,(char[26]){}); p;}),"\n"),ctime_r(&tm, (char[26]){}),content.c_str());
                f << "[WRN] (" << std::put_time(t, "%c") << ") " << content << std::endl;
                break;
            case err:
                fprintf(stderr,"\033[31;1m[ERR]\033[0;31m (%.*s) %s\033[0m\n",(int)strcspn(({char *p=ctime_r(&tm,(char[26]){}); p;}),"\n"),ctime_r(&tm, (char[26]){}),content.c_str());
                f << "[ERR] (" << std::put_time(t, "%c") << ") " << content << std::endl;
                break;
            case exc:
                fprintf(stderr,"\033[31;1m[EXCEPTION]\033[0;31m (%.*s) %s\033[0m\n",(int)strcspn(({char *p=ctime_r(&tm,(char[26]){}); p;}),"\n"),ctime_r(&tm, (char[26]){}),content.c_str());
                f << "\n    AN UNEXPECTED EXCEPTION OCCURED!\n       (" << std::put_time(t, "%c") << ")\n\nException details: \n" << content << std::endl;
                break;
            case dbg:
                if (DEBUG) {
                    printf("\033[95;1m[DBG]\033[0;35m (%.*s) %s\n",(int)strcspn(({char *p=ctime_r(&tm,(char[26]){}); p;}),"\n"),ctime_r(&tm, (char[26]){}),content.c_str());
                    f << "[DBG] (" << std::put_time(t, "%c") << ") " << content << std::endl;
                }
                break;
            default:
                printf("[INF] (%.*s) %s\n",(int)strcspn(({char *p=ctime_r(&tm,(char[26]){}); p;}),"\n"),ctime_r(&tm, (char[26]){}),content.c_str());
                f << "[INF] (" << std::put_time(t, "%c") << ") " << content << std::endl;
                break;
            
        }
        f.close();
    } else {
        switch (type) {
            case inf:
                printf("\033[0;1m[INF]\033[0;90m (%.*s)\033[0m %s\n",(int)strcspn(({char *p=ctime_r(&tm,(char[26]){}); p;}),"\n"),ctime_r(&tm, (char[26]){}),content.c_str());
                break;
            case wrn:
                printf("\033[33;1m[WRN]\033[0;33m (%.*s) %s\033[0m\n",(int)strcspn(({char *p=ctime_r(&tm,(char[26]){}); p;}),"\n"),ctime_r(&tm, (char[26]){}),content.c_str());
                break;
            case err:
                fprintf(stderr,"\033[31;1m[ERR]\033[0;31m (%.*s) %s\033[0m\n",(int)strcspn(({char *p=ctime_r(&tm,(char[26]){}); p;}),"\n"),ctime_r(&tm, (char[26]){}),content.c_str());
                break;
            case exc:
                fprintf(stderr,"\n\033[31;1mAN UNEXPECTED EXCEPTION OCCURED!\033[0;31m (%.*s)\n\033[4mException details:\033[24m\n%s\033[0m\n",(int)strcspn(({char *p=ctime_r(&tm,(char[26]){}); p;}),"\n"),ctime_r(&tm, (char[26]){}),content.c_str());
                break;
            case dbg:
                if (DEBUG) {
                    printf("\033[95;1m[DBG]\033[0;35m (%.*s) %s\n",(int)strcspn(({char *p=ctime_r(&tm,(char[26]){}); p;}),"\n"),ctime_r(&tm, (char[26]){}),content.c_str());
                }
                break;
            default:
                printf("[INF] (%.*s) %s\n",(int)strcspn(({char *p=ctime_r(&tm,(char[26]){}); p;}),"\n"),ctime_r(&tm, (char[26]){}),content.c_str());
                break;
            
        }
    }
}