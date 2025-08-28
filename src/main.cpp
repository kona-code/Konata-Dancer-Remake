#include "Interface.h"
#include <iostream>
#include <atomic>
#include <thread>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "Tray.h" // errors can be false-positive - makefile should cover them

Interface ui;
std::atomic<bool> uiRunning{true};

static bool open_url(const std::string &url) {
    if (url.empty()) return false;

#if defined(_WIN32) || defined(_WIN64)
    // use ShellExecuteW using a utf-16 string
    std::wstring wurl = utf8_to_wstring(url);
    HINSTANCE result = ShellExecuteW(NULL, L"open", wurl.c_str(), NULL, NULL, SW_SHOWNORMAL);
    // ShellExecute returns value > 32 for success
    return reinterpret_cast<intptr_t>(result) > 32;
#else
    // prefer xdg-open (widely available)
    pid_t pid = fork();
    if (pid == -1) return false; // fork failed
    if (pid == 0) {
        // child
        execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
        // if xdg-open isn't available, try sensible-browser (rare) then exit
        execlp("sensible-browser", "sensible-browser", url.c_str(), static_cast<char*>(nullptr));
        _exit(EXIT_FAILURE);
    }
    return true;
#endif
}

static inline void enable_stats(struct tray_menu *item) {
    ui.stats = !ui.stats;
}
static inline void support(struct tray_menu *item) {
    if (!open_url("https://nightvoid.com/support")) {
        std::cout << "[ERROR] Could not open support email URL!" << std::endl;
    } 
    else {  std::cout << "[INFO] Email URL oppened!" << std::endl;  }
}
static inline void original(struct tray_menu *item) {
    if (!open_url("https://www.moddb.com/groups/anime-fans-of-moddb/downloads/konata-desktop-dancer")) {
        std::cout << "[ERROR] Could not open URL!" << std::endl;
    } 
    else {  std::cout << "[INFO] ModDB URL oppened!" << std::endl;  }
}

static inline void konacode(struct tray_menu *item) {
    if (!open_url("https://konacode.com/")) {
        std::cout << "[ERROR] Could not open URL!" << std::endl;
    } 
    else {  std::cout << "[INFO] URL oppened successfully!" << std::endl;  }
}

static inline void github(struct tray_menu *item) {
    if (!open_url("https://github.com/kona-code")) {
        std::cout << "[ERROR] Could not open URL!" << std::endl;
    } 
    else {  std::cout << "[INFO] URL oppened successfully!" << std::endl;  }
}

static inline void nightvoid(struct tray_menu *item) {
    if (!open_url("https://nightvoid.com/")) {
        std::cout << "[ERROR] Could not open URL!" << std::endl;
    } 
    else {  std::cout << "[INFO] URL oppened successfully!" << std::endl;  }
}

static inline void software(struct tray_menu *item) {
    if (!open_url("https://software.nightvoid.com/")) {
        std::cout << "[ERROR] Could not open URL!" << std::endl;
    } 
    else {  std::cout << "[INFO] URL oppened successfully!" << std::endl;  }
}

static inline void openui(struct tray_menu *item) {
    Interface::Show();
}
static inline void kill(struct tray_menu *item) {
    std::exit(EXIT_SUCCESS);
}

static inline void shutdown(struct tray_menu *item) {
    uiRunning = false;
    
    std::exit(EXIT_SUCCESS);
}
static struct tray tray = {
    .icon = TRAY_ICON1,
    .menu =
        (struct tray_menu[]){
            {.text = "Force-Show Window", .cb = openui},
            {.text = "Enable Vk/SDL2 statistics", .cb = enable_stats},
            {.text = "(Original) WIN Version", .cb = original},
            {.text = "-"},
            {.text = "konacode",
             .submenu =
                 (struct tray_menu[]){
                     {.text = "website", .cb = konacode},
                     {.text = "GitHub", .cb = github},
                     {.text = NULL}}},
              {.text = "NightVoid",
               .submenu =
                 (struct tray_menu[]){
                     {.text = "website", .cb = nightvoid},
                     {.text = "software", .cb = software},
                     {.text = "support", .cb = support},
              {.text = NULL}}},
            {.text = "-"},
            {.text = "Kill", .cb = kill},
            {.text = "Quit", .cb = shutdown},
            {.text = NULL}},
};

int main() {

    // developer notice
    std::cout << 
"\n\n     occ.                            .klccc.                \n"
    "    dcc                           o0xlccccd00O00KXNk.       \n"
    "   ,cl'              .OXK00OOOOOkoccccccc'......;ccc::clxO, \n"
    "   .ld.         .KOxocccccccccllccccccccc:...               \n"
    "    'd,       0xlccccccccccccclccccccccc:....;kKx           \n"
    "      k.   .0occcccccccccccccc:cccclccc;..',:ccccoO0                 __                                    __           \n"
    "        l xocccccccccccccccccc:;:lcclc;;:cccccccccccoOc             / /______  ____  ____ __________  ____/ /__         \n"
    "         xccccccccccccccccccccc:';lcclcccccccccccclcccckd          / //_/ __ \\/ __ \\/ __ `/ ___/ __ \\/ __  / _ \\    \n"
    "        occcccccccccccccccclcc:c:.;lccdlcccccccccccllcccck,       / ,< / /_/ / / / / /_/ / /__/ /_/ / /_/ /  __/        \n"
    "       ,cccccc;.,;;,,',:ccllccc:c:.cllc:;ccccllcccccl,  .clO     /_/|_|\\____/_/ /_/\\__,_/\\___/\\____/\\__,_/\\___/   \n"
    "       lcccc;.......,:clccl':colcc,,c.:c...;cccooccccl:    '\n"
    "       lcc:.....,;ccccclcc;..;cdocc,c,.,:....,:cd .cccl:                   Software developed by kona                   \n"
    "       cc;.',;lccclccccocc....;lldc::c..::cox.l 'x  ,cco.          ┌─────────────────────────────────────────┐          \n"
    "       ll:clcclccoccccccc:.....:;;occc'oO00dc.dl      cco              website: https://konacode.com/                   \n"
    "      olclocclccolcccc,:c.......l.,occ,,kOxk..dd;      .l.             my projects: https://nightvoid.com/              \n"
    "     k.,clccclcldcccc:.l;:ox0o..',.'oc;..::l .doo'                     github: https://github.com/kona-code/            \n"
    "   '.  ,lccccocddcccc;o000Okk:......,l;..:;l..cdlo,         \n"
    "       ,dcccoxldocccc.:oOxxkld ......:;.......'do:oc                   contact me here:                                 \n"
    "       :lccdddodoccdd;... ,:,d,...............cddc:lk                  Discord: konacode                                \n"
    "       llcddddxddcdddo,...coc;........,,..... lddd::.x                 Email: kona@nightvoid.com                        \n"
    "      'lldddddxddlddddl,...........;,'...';:    ddo:o.             └─────────────────────────────────────────┘          \n"
    "      olddddddddxloodddl;..............,c:c;     :dlcl      \n"
    "     dccdddddddxoooolodl...:,,,''';,;ldlllc       .ocl      \n"
    "    k:ccc:clodlllllllcl'..,ll:;,,,,,,odl:oo.       .cc      \n"
    "  'd;::.......',;::::c;...doccllc;,,'.dl;o:,        :c      \n"
    " ll,:odl::;,'.............:ldolloo:.  :cco,.        .'      \n"
    "Oc:loddoddxc,';'....'.....ccclolclod:.:cdc'                 \n"
    "llldxdoxo:,'..;''',;;'''':ddllooodllllldl;.                 \n"
    "dodkxdko'''''':;;;;;;,,;xkkxo:clllodooxoc;x                 \n"
    "ddxxdkO;,,,,,;cc:cc;;;;lOkOxl,;lllloollllllo                \n";
        
    std::thread trayThread([&](){
        tray_init(&tray);
            while (tray_loop(1) == 0);
        tray_exit();
    });
    ui.Initialize();
    ui.Render(&uiRunning);
    try {
        if (trayThread.joinable()) {
            trayThread.join();
            std::cout << "[INFO] trayThread destroyed successfully!" << std::endl;
        }
    } 
    catch (...) { 
        std::cout << "[ERROR] Tray thread \"trayThread\" is unjoinable!" << std::endl;
    }
}