#include <iostream>
#include <atomic>
#include "Interface.h"

Interface ui;
std::atomic<bool> uiRunning{true};

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
        
    ui.Initialize();
    ui.Render(&uiRunning);
}