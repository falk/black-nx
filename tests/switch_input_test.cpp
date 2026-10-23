// Exercise the real input backend against a fake HID service. This checks
// controller transitions and emitted motor commands, not physical vibration.
#include <switch.h>
#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include <cassert>
#include <cmath>
#include <cstdio>
namespace app {const GameInfo kGame{};}
namespace logx {void Write(Level,const char*,...) {}}
int main() {
 using namespace hidmock;
 ports[0].connected=true;
 input::Init();
 assert(sends.size()==1 && sends.back().size()==2);
 input::Rumble(0,65535,32768);
 auto a=sends.back();
 assert(a[0].amp_low==1 && a[0].amp_high==0);
 assert(a[1].amp_low==0 && std::fabs(a[1].amp_high-.5000076f)<.00001f);
 auto n=sends.size(); input::Rumble(0,65535,32768); assert(sends.size()==n);
 // A new controller style must receive unchanged active motor values.
 ports[0].style=HidNpadStyleTag_NpadJoyDual; input::Poll(0);
 assert(sends.size()==++n && sends.back()[0].amp_low==1);
 ports[0].connected=false; input::Poll(0); assert(sends.size()==n);
 ports[0].connected=true; input::Poll(0);
 assert(sends.size()==++n && sends.back()[0].amp_low==0 && sends.back()[1].amp_high==0);
 // Device-service failures retry after a delay, without dropping a request.
 fail=true; ports[0].style=HidNpadStyleTag_NpadHandheld; input::Poll(0);
 input::Rumble(0,30000,40000); auto tries=initializations;
 input::Poll(0); assert(initializations==tries && sends.size()==n);
 fail=false; tick+=2000000000ull; input::Poll(0);
 assert(sends.size()==++n && sends.back()[0].amp_low>0 && sends.back()[1].amp_high>0);
 input::Rumble(1,65535,65535); assert(sends.size()==n);
 // Both trigger and D-pad mappings reach the game's report independently.
 ports[0].buttons=HidNpadButton_ZL|HidNpadButton_Up|HidNpadButton_Right;
 auto p=input::Poll(0); assert(p.analog[6]==255 && p.analog[7]==0 && p.buttons==9);
 puts("PASS Switch input: motor routing, unchanged-state suppression, controller handoff, reconnect, service retry and button mapping");
}
