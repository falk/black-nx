#pragma once
#include <cstdint>
#include <vector>
using u32=uint32_t; using u64=uint64_t; using Result=uint32_t;
#define R_SUCCEEDED(r) ((r)==0)
#define R_FAILED(r) ((r)!=0)
enum HidNpadIdType {HidNpadIdType_No1,HidNpadIdType_No2,HidNpadIdType_No3,HidNpadIdType_No4,HidNpadIdType_Handheld};
enum HidNpadStyleTag {HidNpadStyleTag_NpadFullKey=1,HidNpadStyleTag_NpadHandheld=2,HidNpadStyleTag_NpadJoyDual=4};
constexpr u32 HidNpadStyleSet_NpadStandard=7;
enum : u64 {HidNpadButton_Up=1,HidNpadButton_Down=2,HidNpadButton_Left=4,HidNpadButton_Right=8,
 HidNpadButton_Plus=16,HidNpadButton_Minus=32,HidNpadButton_StickL=64,HidNpadButton_StickR=128,
 HidNpadButton_A=256,HidNpadButton_B=512,HidNpadButton_X=1024,HidNpadButton_Y=2048,
 HidNpadButton_R=4096,HidNpadButton_L=8192,HidNpadButton_ZL=16384,HidNpadButton_ZR=32768};
struct HidAnalogStickState {int32_t x=0,y=0;};
struct PadState {int port=0;};
struct HidVibrationDeviceHandle {u32 id=0;};
struct HidVibrationValue {float amp_low=0,freq_low=0,amp_high=0,freq_high=0;};
namespace hidmock {
struct Port {bool connected=false; u32 style=HidNpadStyleTag_NpadFullKey; u64 buttons=0;};
inline Port ports[4]; inline u64 tick=0; inline bool fail=false; inline unsigned initializations=0;
inline std::vector<std::vector<HidVibrationValue>> sends;
}
inline u64 armGetSystemTick(){return hidmock::tick+=2000000;}
inline u64 armNsToTicks(u64 n){return n;}
inline u64 armTicksToNs(u64 n){return n;}
inline void padConfigureInput(int,u32){}
inline void padInitialize(PadState* p,HidNpadIdType id){p->port=int(id);}
inline void padInitialize(PadState* p,HidNpadIdType id,HidNpadIdType){p->port=int(id);}
inline void padUpdate(PadState*){}
inline bool padIsConnected(const PadState* p){return hidmock::ports[p->port].connected;}
inline u64 padGetButtons(const PadState* p){return hidmock::ports[p->port].buttons;}
inline u32 padGetStyleSet(const PadState* p){return hidmock::ports[p->port].style;}
inline HidAnalogStickState padGetStickPos(const PadState*,int){return {};}
inline Result hidInitializeVibrationDevices(HidVibrationDeviceHandle*,int,HidNpadIdType,HidNpadStyleTag){++hidmock::initializations;return hidmock::fail?1:0;}
inline Result hidSendVibrationValues(const HidVibrationDeviceHandle*,const HidVibrationValue* v,int n){hidmock::sends.emplace_back(v,v+n);return 0;}
