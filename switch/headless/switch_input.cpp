// Controllers on Horizon: HID pads read into the console controller's report (core/input.h), the Xbox motors on HD
// rumble.
//
// Port 0 is the handheld Joy-Cons or player 1's controller (a detached Joy-Con pair or a Pro Controller), ports 1 to 3
// players 2 to 4. Buttons go by their labels, as in the CoD4 port: A confirms and B goes back, as on the Xbox.
//   A B X Y -> A B X Y      R -> black      L -> white      ZL ZR -> left and right trigger, full press
//   + -> START      - -> BACK      stick clicks -> left and right thumb      d-pad -> d-pad
// A stick inside a small radius reads as centred (Joy-Con sticks drift); outside it the reading passes through
// unscaled, so the game's own dead zone still applies as on the Xbox.
#include <switch.h>

#include <algorithm>
#include <mutex>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"

namespace input {

namespace {

constexpr float kDeadZone = 0.12f;  // of full deflection

std::mutex g_lock;
bool g_init = false;
PadState g_pads[4];
Pad g_state[4];
uint64_t g_read_tick = 0;

void Stick(const HidAnalogStickState& s, int16_t& x, int16_t& y) {
    float fx = float(s.x) / 32767.0f, fy = float(s.y) / 32767.0f;
    if (fx * fx + fy * fy < kDeadZone * kDeadZone) {
        x = y = 0;
        return;
    }
    x = int16_t(std::clamp<int32_t>(s.x, -32767, 32767));
    y = int16_t(std::clamp<int32_t>(s.y, -32767, 32767));  // HID's y is up, as XInput's
}

Pad Read(PadState& p) {
    Pad o;
    padUpdate(&p);
    o.connected = padIsConnected(&p);
    if (!o.connected) return o;
    const u64 b = padGetButtons(&p);
    constexpr u64 kButtons[8] = {HidNpadButton_Up,   HidNpadButton_Down,  HidNpadButton_Left,    HidNpadButton_Right,
                                 HidNpadButton_Plus, HidNpadButton_Minus, HidNpadButton_StickL, HidNpadButton_StickR};
    for (int i = 0; i < 8; i++)
        if (b & kButtons[i]) o.buttons |= uint16_t(1u << i);
    // A B X Y black white, left trigger, right trigger
    constexpr u64 kAnalog[8] = {HidNpadButton_A, HidNpadButton_B, HidNpadButton_X,  HidNpadButton_Y,
                                HidNpadButton_R, HidNpadButton_L, HidNpadButton_ZL, HidNpadButton_ZR};
    for (int i = 0; i < 8; i++)
        if (b & kAnalog[i]) o.analog[i] = 0xFF;
    Stick(padGetStickPos(&p, 0), o.lx, o.ly);
    Stick(padGetStickPos(&p, 1), o.rx, o.ry);
    return o;
}

// rumble: the devices of port 0's current style, sent when the motors change
HidVibrationDeviceHandle g_vib[2];
int g_vib_count = 0;
u32 g_vib_style = 0;
uint64_t g_vib_retry = 0;          // tick before which a failed initialisation is not tried again
uint16_t g_motor[2] = {0, 0};      // what the game asked for
uint16_t g_sent[2] = {0, 0};       // what the motors were last sent
bool g_pending = false;            // g_motor not sent yet (no device ready when it changed)

// the vibration devices of port 0's current style: both motors of a Joy-Con pair, the handheld Joy-Cons or a Pro
// Controller (two actuators; one handle where libnx refuses two), retried at most once a second after a failure
bool VibrationDevices() {
    const u32 styles = padGetStyleSet(&g_pads[0]);
    constexpr u32 kOrder[] = {HidNpadStyleTag_NpadHandheld, HidNpadStyleTag_NpadJoyDual, HidNpadStyleTag_NpadFullKey};
    u32 tag = 0;
    for (u32 t : kOrder)
        if (styles & t) {
            tag = t;
            break;
        }
    if (!tag) return false;
    if (tag == g_vib_style && g_vib_count > 0) return true;
    const uint64_t now = armGetSystemTick();
    if (tag == g_vib_style && now < g_vib_retry) return false;
    g_vib_style = tag;
    HidNpadIdType id = tag == HidNpadStyleTag_NpadHandheld ? HidNpadIdType_Handheld : HidNpadIdType_No1;
    g_vib_count = 2;
    Result rc = hidInitializeVibrationDevices(g_vib, 2, id, HidNpadStyleTag(tag));
    if (R_FAILED(rc) && tag == HidNpadStyleTag_NpadFullKey) {
        g_vib_count = 1;
        rc = hidInitializeVibrationDevices(g_vib, 1, id, HidNpadStyleTag(tag));
    }
    if (R_FAILED(rc)) {
        LOGW("input: rumble unavailable for styles %X (%X), retrying", unsigned(tag), unsigned(rc));
        g_vib_count = 0;
        g_vib_retry = now + armNsToTicks(1000000000);
        return false;
    }
    g_pending = true;
    LOGI("input: rumble on styles %X, %d devices", unsigned(tag), g_vib_count);
    return true;
}

// sends g_motor when it differs from what the motors have; caller holds g_lock
void SendMotors() {
    if (!VibrationDevices()) {
        g_pending = true;
        return;
    }
    if (!g_pending && g_motor[0] == g_sent[0] && g_motor[1] == g_sent[1]) return;
    HidVibrationValue v{};
    v.amp_low = float(g_motor[0]) / 65535.0f;
    v.freq_low = 160.0f;
    v.amp_high = float(g_motor[1]) / 65535.0f;
    v.freq_high = 320.0f;
    HidVibrationValue values[2] = {v, v};
    if (g_vib_count == 2) {
        values[0].amp_high = 0;
        values[1].amp_low = 0;
    }
    Result sent = hidSendVibrationValues(g_vib, values, g_vib_count);
    if (R_SUCCEEDED(sent)) {
        g_sent[0] = g_motor[0];
        g_sent[1] = g_motor[1];
        g_pending = false;
    } else {
        LOG_FIRST(4, "input: rumble send failed (%X)", unsigned(sent));
        g_pending = true;
    }
}

// all ports at most once a millisecond, however many threads read
void Update() {
    const uint64_t now = armGetSystemTick();
    if (g_read_tick && armTicksToNs(now - g_read_tick) < 1000000) return;
    g_read_tick = now;
    for (int i = 0; i < 4; i++) {
        bool was = g_state[i].connected;
        g_state[i] = Read(g_pads[i]);
        if (i == 0 && g_state[i].connected != was) {
            g_vib_count = 0;
            g_vib_style = 0;
            g_pending = true;
            if (!g_state[i].connected) g_motor[0] = g_motor[1] = 0;
        }
        if (g_state[i].connected != was)
            LOGI("input: controller %d %s, styles %X", i + 1, g_state[i].connected ? "connected" : "disconnected",
                 unsigned(padGetStyleSet(&g_pads[i])));
    }
    if (g_state[0].connected && (g_pending || padGetStyleSet(&g_pads[0]) != g_vib_style)) SendMotors();
}

}  // namespace

void Init() {
    std::lock_guard lock(g_lock);
    if (g_init) return;
    g_init = true;
    padConfigureInput(4, HidNpadStyleSet_NpadStandard);
    padInitialize(&g_pads[0], HidNpadIdType_No1, HidNpadIdType_Handheld);
    padInitialize(&g_pads[1], HidNpadIdType_No2);
    padInitialize(&g_pads[2], HidNpadIdType_No3);
    padInitialize(&g_pads[3], HidNpadIdType_No4);
    g_pending = true;
    Update();
    LOGI("input: HID pads, port 1 %s", g_state[0].connected ? "connected" : "waiting for a controller");
}

Pad Poll(int port) {
    Pad p;
    {
        std::lock_guard lock(g_lock);
        if (g_init && port >= 0 && port < 4) {
            Update();
            p = g_state[port];
        }
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

// the Xbox's left motor is the heavy low one, the right the light high one
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    if (port != 0) return;
    std::lock_guard lock(g_lock);
    if (!g_init) return;
    if (left || right)
        LOG_FIRST(8, "input: game rumble left=%u right=%u", unsigned(left), unsigned(right));
    g_motor[0] = left;
    g_motor[1] = right;
    SendMotors();
}

}  // namespace input
