#include "logger.h"
#include "../core.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <time.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <limits.h>
  #include <shlobj.h>
#endif

static std::ofstream logfile;
static bool debug;

const static std::filesystem::path getpath() {
#ifdef _WIN32
    PWSTR knownPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &knownPath)) && knownPath) {
        int len = WideCharToMultiByte(CP_UTF8, 0, knownPath, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8; utf8.resize(len ? len-1 : 0);
        if (len) WideCharToMultiByte(CP_UTF8, 0, knownPath, -1, &utf8[0], len, nullptr, nullptr);
        CoTaskMemFree(knownPath);
        std::filesystem::path p(utf8);
        p /= "konacode";
        p /= konacore::project;
        p /= "konanix-runtime.log";
        return p;
    }
    if (const char* up = std::getenv("USERPROFILE")) {
        std::filesystem::path p(up);
        p /= "AppData";
        p /= "Local";
        p /= "konacode";
        p /= konacore::project;
        p /="konanix-runtime.log";
        return p;
    }
#endif
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".config/konacode"/konacore::project/"konanix-runtime.log";
    }
    return std::filesystem::path("konanix-runtime.log");
}

void logger::initialize(const bool& log_to_file, const bool& enable_debug) {
    std::filesystem::path logpath;
    if (log_to_file) {
        logpath = getpath();
        if (!logpath.parent_path().empty())
            std::filesystem::create_directories(logpath.parent_path());
        logfile.open(logpath, std::ios::app);
        if (!std::filesystem::exists(logpath)) {
            printf("\033[0m[INF]\033[0m <logger> Created missing file \"%s\".\n", logpath.c_str());
            logfile << "[INF] <logger> Created missing file " << logpath << ".\n";
        }
        printf("\033[0m[INF]\033[0m <logger> Logging to \"%s\".\n", logpath.c_str());
    } else { printf("\033[0m[INF]\033[0m <logger> Logging to file has been disabled!\n"); }
    if (enable_debug) {
        printf("\033[0m[INF][0m[38;2;184;138;237m <logger> Debug mode has been enabled! You will see additional detailed logs.\033[0m\n");
        debug = true;
    }
}


void logger::log(const std::string_view content, const LogType type) {
    const time_t tm = time(0);
    // const struct tm* t = localtime(&tm);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S %d/%m/%Y", localtime(&tm));
// #ifdef _WIN32
//     localtime_s(&tm,t);
// #else
//     localtime_r(&tm,t);
// #endif
    if (logfile.is_open()) {
        switch (type) {
            case inf:
                printf("\033[0m[INF]\033[0;90m (%s)\033[0m %.*s\n",tbuf,(int)content.size(),content.data());
                logfile << "[INF] (" << tbuf << ") " << content << '\n';
                break;
            case wrn:
                printf("[0m[38;2;234;188;110m[WRN] (%s) %.*s\033[0m\n",tbuf,(int)content.size(),content.data());
                logfile << "[WRN] (" << tbuf << ") " << content << '\n';
                break;
            case err:
                fprintf(stderr,"[38;2;225;105;138m[ERR] (%s) %.*s\033[0m\n",tbuf,(int)content.size(),content.data());
                logfile << "[ERR] (" << tbuf << ") " << content << '\n';
                break;
            case exc:
                fprintf(stderr,"\n\n[38;2;225;105;138m[EXC] AN EXCEPTION OCCURED AT %s!\n\033[4mException details:\033[24m\n%.*s\033[0m\n\n",tbuf,(int)content.size(),content.data());
                logfile << "\n    AN EXCEPTION OCCURED AT " << tbuf << "!\n\nException details: \n" << content << '\n';
                break;
            case dbg:
                if (debug) {
                    printf("[0m[38;2;184;138;237m[DBG] (%s) [0m[38;2;184;148;237m%.*s\n",tbuf,(int)content.size(),content.data());
                    // printf("[0m[38;2;184;138;237m[DBG][0m[38;2;144;108;197m (%s) [0m[38;2;184;148;237m%.*s\n",tbuf,(int)content.size(),content.data());
                    logfile << "[DBG] (" << tbuf << ") " << content << '\n';
                }
                break;
            default:
                printf("[INF] (%s) %.*s\n",tbuf,(int)content.size(),content.data());
                logfile << "[INF] (" << tbuf << ") " << content << '\n';
                break;
            
        }
    } else {
        switch (type) {
            case inf:
                printf("\033[0m[INF]\033[0;90m (%s)\033[0m %.*s\n",tbuf,(int)content.size(),content.data());
                break;
            case wrn:
                printf("[0m[38;2;234;188;110m[WRN] (%s) %.*s\033[0m\n",tbuf,(int)content.size(),content.data());
                break;
            case err:
                fprintf(stderr,"[38;2;225;105;138m[ERR] (%s) %.*s\033[0m\n",tbuf,(int)content.size(),content.data());
                break;
            case exc:
                fprintf(stderr,"\n\n[38;2;225;105;138m[EXC] AN EXCEPTION OCCURED AT %s!\n\033[4mException details:\033[24m\n%.*s\033[0m\n\n",tbuf,(int)content.size(),content.data());
                break;
            case dbg:
                if (debug)
                    printf("[0m[38;2;184;138;237m[DBG] (%s) [0m[38;2;184;148;237m%.*s\n",tbuf,(int)content.size(),content.data());
                    // printf("[0m[38;2;184;138;237m[DBG][0m[38;2;144;108;197m (%s) [0m[38;2;184;148;237m%.*s\n",tbuf,(int)content.size(),content.data());
                break;
            default:
                printf("[INF] (%s) %.*s\n",tbuf,(int)content.size(),content.data());
                break;
            
        }
    }
}
