// FH3 Data Out - sends Forza Horizon "Data Out" telemetry for Forza Horizon 3.
//
// FH3 has no Data Out option. This reads the player's car physics from the running
// game (read-only, from outside its sandbox) and sends the same 324-byte UDP packet
// FH4/FH5/FH6 send, so SimHub, motion rigs and dashboards work with FH3.
//
// Field offsets come from FH4's own Data Out builder, matched to FH3's code; see
// dynmap.py in the research folder. Build: FH3 1.0.125.x (forza_x64_release_final.exe).
//
// Usage: FH3DataOut.exe [host:port ...] [--rate N]
//        default 127.0.0.1:8888 at 60 Hz. Up to 8 destinations.
//        FH3DataOut.ini beside the exe may hold "dest=host:port" and "rate=N" lines.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include <sddl.h>
#include <userenv.h>
#include <securityappcontainer.h>
#include <combaseapi.h>
#include <atomic>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

static const wchar_t* kExe = L"forza_x64_release_final.exe";
static const uint32_t kImageSize = 0x7398A00;  // 1.0.125.2 / 1.0.125.3

// ---- FH3 map (interface-relative unless noted) ----
namespace m {
const uint64_t kForza = 0x5CB5BF0;      // exe rva: CForza*
const uint64_t kGameMgr = 0x50;          // CForza -> CGameManager
const uint64_t kPause = 0x28;            // CGameManager -> pause byte
const uint64_t kSim = 0x98;              // CGameManager -> ForzaSimulation
const uint64_t kCarList = 0x298;         // sim -> list; ForzaCar* vector at +0x18..+0x20
const uint64_t kVecBegin = 0x18;
const uint64_t kCarVt = 0x51100F0;       // ForzaCar vtable rva
const uint64_t kCarDyn = 0x71E8;         // ForzaCar -> CCarDynamics interface
const uint64_t kIfaceVt = 0x54C10F8;     // interface vtable rva

const int kRpmMax = 0x1D4, kRpmIdle = 0x5C8, kRpm = 0x148;
const int kVel = -0x4A0, kAngVel = -0x490, kPos = -0x480, kRows = -0x440, kAccel = 0xDC0;
const int kSpeed = 0x12D8, kPower = 0x194, kTorque = 0x18C;
const int kDrivetrain = 0xC48, kCylinders = 0x1C0, kGear = 0xAA8, kInputs = 0x1298;
const int kFuelA = 0x17C, kFuelB = 0xA8C;
const int kTurboFlag = 0xA2C, kTurbo = 0x170, kScFlag = 0xA5C, kSc = 0x178, kNa = 0x1730;
const int kWheel = 0x1490;
const int kSusp = 0x1760, kSuspRest = 0x1C84, kSuspMin = 0x1BBC, kSuspMax = 0x1BC0;
const int kSlipRatio = 0x1984, kSlipAngle = 0x199C, kSlipComb = 0x19F4, kRot = 0x176C;
const int kPuddle = 0x1B54;
const int kTempCore = 0x17D0, kTempA = 0x17C4, kTempB = 0x17C8, kTempC = 0x17CC;
const int kWheelOrder[4] = {0, 1, 3, 2};  // packet FL, FR, RL, RR -> game wheel index

// Rumble. Materials manager: surface-type names (std::string x 0x20) at +0x70..+0x78,
// material table at [+8], 0x3C0 per material, rumble float at +0x284.
const uint64_t kMatMgr = 0x5CCD340;      // exe rva
const int kMatStride = 0x3C0, kMatRumble = 0x284;
const int kContact = 0x18E0;             // wheel contact; byte +1 = material
const int kContactFlag = 0x1B50;         // byte: wheel has contact
const int kSurface = 0x1930;             // wheel -> surface object; dword +0 = surface type

// Car id: dword [[ForzaCar+0x108]] = FH3's car database id (1625 = LAM_Urus_12).
const uint64_t kCarInfo = 0x108;
// Race: CScoreManager [exe+0x5CC80A0]; entry* vector at +0x70..+0x78 in race order,
// empty in free roam. The player's entry has [+0x18] == ForzaCar.
const uint64_t kScoreMgr = 0x5CC80A0;
const int kRcCar = 0x18, kRcClock = 0x60, kRcStart = 0x70, kRcBest = 0x80, kRcLast = 0x88;
const int kRcLapsDone = 0x94, kRcPosition = 0xA0, kRcLapTime = 0xB0;

const int kSpanLo = -0x4B0;
const int kSpanHi = kSuspRest + 3 * kWheel + 0x10;
}  // namespace m

#pragma pack(push, 1)
struct Packet {
    int32_t IsRaceOn;
    uint32_t TimestampMS;
    float EngineMaxRpm, EngineIdleRpm, CurrentEngineRpm;
    float Accel[3], Vel[3], AngVel[3];
    float Yaw, Pitch, Roll;
    float SuspNorm[4], SlipRatio[4], WheelRot[4];
    int32_t Rumble[4], Puddle[4];
    float SurfaceRumble[4], SlipAngle[4], SlipComb[4], SuspMeters[4];
    int32_t CarOrdinal, CarClass, CarPI, Drivetrain, Cylinders;
    uint32_t CarGroup;
    float SmashVelDiff, SmashMass;
    float Pos[3];
    float Speed, Power, Torque;
    float TireTemp[4];
    float Boost, Fuel, Distance, BestLap, LastLap, CurrentLap, RaceTime;
    uint16_t LapNumber;
    uint8_t RacePosition, Throttle, Brake, Clutch, HandBrake, Gear;
    int8_t Steer, DrivingLine, AiBrakeDiff;
    uint8_t pad;
};
#pragma pack(pop)
static_assert(sizeof(Packet) == 324, "Horizon Data Out packet is 324 bytes");
static_assert(offsetof(Packet, CarOrdinal) == 212, "CarOrdinal at 212");
static_assert(offsetof(Packet, Pos) == 244, "PositionX at 244");

// ---- process access ----
struct Game {
    HANDLE h = nullptr;
    DWORD pid = 0;
    uint64_t base = 0;
    uint32_t size = 0;

    bool read(uint64_t a, void* out, size_t n) const {
        SIZE_T got = 0;
        return a && ReadProcessMemory(h, (LPCVOID)a, out, n, &got) && got == n;
    }
    uint64_t u64(uint64_t a) const { uint64_t v = 0; return read(a, &v, 8) ? v : 0; }
    bool alive() const { DWORD code = 0; return h && GetExitCodeProcess(h, &code) && code == STILL_ACTIVE; }
    void close() { if (h) CloseHandle(h); h = nullptr; pid = 0; base = 0; }
};

static DWORD FindPid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof(pe)};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, kExe) == 0) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

static bool Attach(Game& g) {
    DWORD pid = FindPid();
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModulesEx(h, mods, sizeof(mods), &need, LIST_MODULES_64BIT)) { CloseHandle(h); return false; }
    wchar_t name[MAX_PATH];
    for (DWORD i = 0; i < need / sizeof(HMODULE); ++i) {
        if (GetModuleBaseNameW(h, mods[i], name, MAX_PATH) && _wcsicmp(name, kExe) == 0) {
            MODULEINFO mi{};
            if (!GetModuleInformation(h, mods[i], &mi, sizeof(mi))) break;
            g.h = h; g.pid = pid; g.base = (uint64_t)mi.lpBaseOfDll; g.size = mi.SizeOfImage;
            return true;
        }
    }
    CloseHandle(h);  // modules not listed yet (game still starting)
    return false;
}

// ---- the player's car ----
// 0 = not available, else the CCarDynamics interface address. *paused set when known.
static uint64_t PlayerDynamics(const Game& g, int* paused, uint64_t* carOut) {
    *paused = -1;
    *carOut = 0;
    uint64_t forza = g.u64(g.base + m::kForza);
    uint64_t mgr = forza ? g.u64(forza + m::kGameMgr) : 0;
    if (!mgr) return 0;
    uint8_t p = 0;
    if (g.read(mgr + m::kPause, &p, 1)) *paused = p != 0;
    uint64_t sim = g.u64(mgr + m::kSim);
    uint64_t list = sim ? g.u64(sim + m::kCarList) : 0;
    if (!list) return 0;
    uint64_t vec[2];
    if (!g.read(list + m::kVecBegin, vec, sizeof(vec)) || vec[1] <= vec[0] || vec[1] - vec[0] > 8 * 64) return 0;
    // Free roam and races both hold only the player here (AI cars live elsewhere).
    for (uint64_t at = vec[0]; at < vec[1]; at += 8) {
        uint64_t car = g.u64(at);
        if (!car || g.u64(car) != g.base + m::kCarVt) continue;
        uint64_t iface = g.u64(car + m::kCarDyn);
        if (iface && g.u64(iface) == g.base + m::kIfaceVt) { *carOut = car; return iface; }
    }
    return 0;
}

struct Block {
    std::vector<uint8_t> buf = std::vector<uint8_t>(m::kSpanHi - m::kSpanLo);
    template <class T> T at(int off) const { T v; memcpy(&v, &buf[off - m::kSpanLo], sizeof(T)); return v; }
    float f(int off) const { return at<float>(off); }
    int32_t i(int off) const { return at<int32_t>(off); }
    void v3(int off, float o[3]) const { o[0] = f(off); o[1] = f(off + 4); o[2] = f(off + 8); }
};

static float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static uint8_t U8(float x) { x = std::fmin(std::fmax(x, 0.f), 1.f); return (uint8_t)(x * 255.f + 0.5f); }
static int8_t S8(float x) { x = std::fmin(std::fmax(x, -1.f), 1.f); return (int8_t)(x * 127.f + (x >= 0 ? 0.5f : -0.5f)); }

static bool Plausible(const Block& b) {
    for (int k = 0; k < 3; ++k) {
        float r[3]; b.v3(m::kRows + 0x10 * k, r);
        float n = std::sqrt(Dot(r, r));
        if (!(n > 0.98f && n < 1.02f)) return false;
    }
    float pos[3]; b.v3(m::kPos, pos);
    for (float v : pos) if (!std::isfinite(v) || std::fabs(v) > 1e6f) return false;
    float mx = b.f(m::kRpmMax) * 9.5493f;
    return mx > 1000.f && mx < 30000.f;
}

static void Fill(const Block& b, Packet& p) {
    const float kRpm = 9.5493f, kAtm = 14.7f;
    float rows[3][3];
    for (int k = 0; k < 3; ++k) b.v3(m::kRows + 0x10 * k, rows[k]);
    auto local = [&](int off, float out[3]) {
        float v[3]; b.v3(off, v);
        for (int k = 0; k < 3; ++k) out[k] = Dot(rows[k], v);
    };
    float mx = b.f(m::kRpmMax);
    p.EngineMaxRpm = mx * kRpm;
    p.EngineIdleRpm = b.f(m::kRpmIdle) * kRpm;
    p.CurrentEngineRpm = std::fmin(std::fmax(b.f(m::kRpm), 0.f), std::fmax(mx, 0.f)) * kRpm;
    local(m::kAccel, p.Accel);
    local(m::kVel, p.Vel);
    local(m::kAngVel, p.AngVel);
    // rows: right, up, forward. Sign set checked against FH4's own packets.
    const float* right = rows[0]; const float* up = rows[1]; const float* fwd = rows[2];
    p.Yaw = std::atan2(fwd[0], fwd[2]);
    p.Pitch = std::asin(std::fmin(std::fmax(-fwd[1], -1.f), 1.f));
    p.Roll = std::atan2(right[1], up[1]);
    for (int c = 0; c < 4; ++c) {
        int o = m::kWheelOrder[c] * m::kWheel;
        float susp = b.f(m::kSusp + o), lo = b.f(m::kSuspMin + o), hi = b.f(m::kSuspMax + o);
        if (lo > hi) std::swap(lo, hi);
        p.SuspNorm[c] = susp <= lo ? 0.f : susp >= hi ? 1.f : (susp - lo) / (hi - lo);
        p.SuspMeters[c] = susp - b.f(m::kSuspRest + o);
        p.SlipRatio[c] = b.f(m::kSlipRatio + o);
        p.SlipAngle[c] = b.f(m::kSlipAngle + o);
        p.SlipComb[c] = b.f(m::kSlipComb + o);
        p.WheelRot[c] = b.f(m::kRot + o);
        p.Puddle[c] = b.f(m::kPuddle + o) > 0.f ? 1 : 0;
        float core = b.f(m::kTempCore + o);
        p.TireTemp[c] = ((core + b.f(m::kTempA + o)) + (core + b.f(m::kTempB + o)) + (core + b.f(m::kTempC + o))) / 3.f;
    }
    int dt = b.i(m::kDrivetrain);
    p.Drivetrain = (dt >= 0 && dt <= 2) ? dt : 0;
    p.Cylinders = b.i(m::kCylinders);
    b.v3(m::kPos, p.Pos);
    p.Speed = b.f(m::kSpeed);
    p.Power = b.f(m::kPower) * 100.f;
    p.Torque = b.f(m::kTorque) * 100.f;
    if (b.i(m::kTurboFlag)) p.Boost = b.f(m::kTurbo) - kAtm;
    else if (b.i(m::kScFlag)) p.Boost = b.f(m::kSc) - kAtm;
    else p.Boost = b.f(m::kNa) * kAtm - kAtm;
    float den = b.f(m::kFuelB);
    p.Fuel = den != 0.f ? b.f(m::kFuelA) / den : 0.f;
    float in[5];
    for (int k = 0; k < 5; ++k) in[k] = b.f(m::kInputs + 4 * k);  // throttle, brake, steer, clutch, handbrake
    p.Throttle = U8(in[0]); p.Brake = U8(in[1]); p.Steer = S8(in[2]); p.Clutch = U8(in[3]); p.HandBrake = U8(in[4]);
    p.Gear = b.at<uint8_t>(m::kGear);
}

// WheelOnRumbleStrip: surface type == index of "RumbleStrip" (FH4's rule).
// SurfaceRumble: 10 * 2 * material rumble, as FH4's builder sends it.
struct Rumble {
    int strip = -2;  // -2 unknown, -1 not found
    double retry = 0.0;

    void Fill(const Game& g, const Block& b, Packet& p, double t) {
        uint64_t mgr = g.u64(g.base + m::kMatMgr);
        if (!mgr) return;
        if (strip == -2 || (strip == -1 && t >= retry)) {
            strip = -1;
            retry = t + 5.0;
            uint64_t v[2];
            if (g.read(mgr + 0x70, v, sizeof(v)) && v[1] > v[0] && v[1] - v[0] <= 0x20 * 256) {
                for (uint64_t s = v[0], k = 0; s < v[1]; s += 0x20, ++k) {
                    uint8_t str[0x20];
                    if (!g.read(s, str, sizeof(str))) break;
                    uint64_t len, cap, ptr;
                    memcpy(&ptr, str, 8); memcpy(&len, str + 0x10, 8); memcpy(&cap, str + 0x18, 8);
                    char text[16] = {};
                    if (len != 11) continue;
                    if (cap >= 0x10) { if (!g.read(ptr, text, 11)) continue; }
                    else memcpy(text, str, 11);
                    if (!memcmp(text, "RumbleStrip", 11)) { strip = (int)k; break; }
                }
            }
        }
        uint64_t table = g.u64(mgr + 8);
        for (int c = 0; c < 4; ++c) {
            int o = m::kWheelOrder[c] * m::kWheel;
            uint64_t surf = b.at<uint64_t>(m::kSurface + o);
            if (strip >= 0 && surf) {
                int32_t type = -1;
                if (g.read(surf, &type, 4)) p.Rumble[c] = type == strip ? 1 : 0;
            }
            if (!table || !b.at<uint8_t>(m::kContactFlag + o)) continue;
            uint8_t mat = b.at<uint8_t>(m::kContact + o + 1);
            float r = 0.f;
            if (g.read(table + (uint64_t)mat * m::kMatStride + m::kMatRumble, &r, 4) && std::isfinite(r))
                p.SurfaceRumble[c] = r * 2.f * 10.f;
        }
    }
};

static void FillIdentityRace(const Game& g, uint64_t car, Packet& p) {
    uint64_t info = g.u64(car + m::kCarInfo);
    uint32_t ord = 0;
    if (info && g.read(info, &ord, 4) && ord > 0 && ord < 1000000) p.CarOrdinal = (int32_t)ord;
    uint64_t mgr = g.u64(g.base + m::kScoreMgr);
    uint64_t v[2];
    if (!mgr || !g.read(mgr + 0x70, v, sizeof(v)) || v[1] <= v[0] || v[1] - v[0] > 8 * 64) return;
    for (uint64_t at = v[0]; at < v[1]; at += 8) {
        uint64_t e = g.u64(at);
        if (!e || g.u64(e + m::kRcCar) != car) continue;
        uint8_t d[0xB8];
        if (!g.read(e, d, sizeof(d))) return;
        auto dbl = [&](int o) { double x; memcpy(&x, d + o, 8); return std::isfinite(x) ? x : 0.0; };
        p.LapNumber = d[m::kRcLapsDone];
        p.RacePosition = d[m::kRcPosition];
        p.BestLap = (float)dbl(m::kRcBest);
        p.LastLap = (float)dbl(m::kRcLast);
        p.CurrentLap = (float)dbl(m::kRcLapTime);
        p.RaceTime = (float)std::fmax(0.0, dbl(m::kRcClock) - dbl(m::kRcStart));
        return;
    }
}

// ---- config ----
struct Dest { sockaddr_in addr{}; std::string text; };

static bool ParseDest(const std::string& s, Dest& d) {
    size_t c = s.rfind(':');
    std::string host = c == std::string::npos ? s : s.substr(0, c);
    int port = c == std::string::npos ? 8888 : atoi(s.c_str() + c + 1);
    if (host.empty() || port <= 0 || port > 65535) return false;
    d.addr.sin_family = AF_INET;
    d.addr.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host.c_str(), &d.addr.sin_addr) != 1) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
        d.addr.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    d.text = host + ":" + std::to_string(port);
    return true;
}

// screen=0: no live readout (the console is not redrawn at all) - FH3 in VR dipped to 36/24 Hz with the tool running and
// the GPU busier, even without the OXRMC bridge (2026-11-14); a test of whether the redraw does it.
// precise=0: no 1 ms system timer and no spin-wait for the packet timing (plain Sleep; packets a little less even).
static const char kVersion[] = "1.0.0";   // (also fh3dataout.rc)
static bool g_screen = true, g_precise = true;
static void ReadIni(std::vector<std::string>& dests, double& rate, bool& oxrmc) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"FH3DataOut.ini");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") || !f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (s.rfind("dest=", 0) == 0) dests.push_back(s.substr(5));
        else if (s.rfind("rate=", 0) == 0) rate = atof(s.c_str() + 5);
        else if (s.rfind("oxrmc=", 0) == 0) oxrmc = atoi(s.c_str() + 6) != 0;
        else if (s.rfind("screen=", 0) == 0) g_screen = atoi(s.c_str() + 7) != 0;
        else if (s.rfind("precise=", 0) == 0) g_precise = atoi(s.c_str() + 8) != 0;
    }
    fclose(f);
}

// ---- OXRMC bridge (VR motion compensation) ----
// FH3 is a UWP app, so it runs in an AppContainer sandbox. An OpenXR layer loaded into it
// (OpenXR-MotionCompensation, OXRMC) can see neither its config in
// %LOCALAPPDATA%\OpenXR-MotionCompensation nor the motion software's rig-pose shared memory,
// because the sandbox redirects both. OXRMC then falls back to defaults: compensation off.
// This side runs outside the sandbox, so it:
//  1. copies OXRMC's global ini into the sandbox's OXRMC folder once, if it is missing there
//     (with load_ref_pose_from_file=0: a locked pose from another runtime's playspace is wrong);
//  2. mirrors each virtual-tracker feed OXRMC knows that exists on this PC into FH3's own
//     object namespace, every millisecond: "motionRigPose" (FlyPT Mover format, also written
//     by SimHub and DR Sim Manager), "YawVRGEFile" (Yaw VR Game Engine) and
//     "SimRacingStudioMotionRigPose" (Sim Racing Studio). A copy is only made once the real
//     feed exists, so OXRMC still reports a missing feed instead of compensating against zeros.
// Nothing happens unless OXRMC has run in FH3 (its sandbox folder exists) or a rig feed exists.
namespace bridge {
std::atomic<DWORD> pid{0};     // FH3 process to feed, 0 = none
std::atomic<int> state{0};     // 0 idle, 1 waiting for a rig feed, 2 mirroring, -1 failed
std::atomic<unsigned> feeds{0};   // bit i: kFeeds[i] is being mirrored
}  // namespace bridge

static const struct { const wchar_t* name; const char* label; } kFeeds[] = {
    {L"motionRigPose", "FlyPT/SimHub/DR Sim"},
    {L"YawVRGEFile", "Yaw VR"},
    {L"SimRacingStudioMotionRigPose", "Sim Racing Studio"},
};

// AppContainer SID string of a process, or "" when it is not sandboxed.
static std::wstring ContainerSid(HANDLE token) {
    DWORD isAc = 0, n = 0;
    if (!GetTokenInformation(token, TokenIsAppContainer, &isAc, sizeof(isAc), &n) || !isAc) return L"";
    BYTE buf[256];
    if (!GetTokenInformation(token, TokenAppContainerSid, buf, sizeof(buf), &n)) return L"";
    LPWSTR s = nullptr;
    if (!ConvertSidToStringSidW(((TOKEN_APPCONTAINER_INFORMATION*)buf)->TokenAppContainer, &s)) return L"";
    std::wstring r(s);
    LocalFree(s);
    return r;
}

static HANDLE OpenToken(DWORD pid) {
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!ph) return nullptr;
    HANDLE tok = nullptr;
    OpenProcessToken(ph, TOKEN_QUERY, &tok);
    CloseHandle(ph);
    return tok;
}

// Step 1: returns a short note for the log, or nullptr when nothing was done.
static const char* OxrmcConfig(DWORD pid) {
    HANDLE tok = OpenToken(pid);
    if (!tok) return nullptr;
    std::wstring sid = ContainerSid(tok);
    CloseHandle(tok);
    if (sid.empty()) return nullptr;
    PWSTR acDir = nullptr;
    if (FAILED(GetAppContainerFolderPath(sid.c_str(), &acDir))) return nullptr;
    std::wstring dst = std::wstring(acDir) + L"\\OpenXR-MotionCompensation";
    CoTaskMemFree(acDir);
    wchar_t la[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) return nullptr;
    std::wstring src = std::wstring(la) + L"\\OpenXR-MotionCompensation\\OpenXR-MotionCompensation.ini";
    if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;  // OXRMC never ran in FH3
    std::wstring dstIni = dst + L"\\OpenXR-MotionCompensation.ini";
    if (GetFileAttributesW(dstIni.c_str()) != INVALID_FILE_ATTRIBUTES) return "OXRMC config already in FH3's sandbox";
    FILE* in = nullptr;
    if (_wfopen_s(&in, src.c_str(), L"rb") || !in) return nullptr;
    std::string text;
    char b[4096];
    for (size_t n; (n = fread(b, 1, sizeof(b), in)) > 0;) text.append(b, n);
    fclose(in);
    // Drop the locked reference pose for this playspace (keeps the _oc variant untouched).
    size_t at = 0;
    while ((at = text.find("load_ref_pose_from_file", at)) != std::string::npos) {
        size_t eol = text.find('\n', at);
        std::string key = text.substr(at, text.find_first_of(" =", at) - at);
        if (key == "load_ref_pose_from_file" && (at == 0 || text[at - 1] == '\n')) {  // a key, not a comment
            text.replace(at, (eol == std::string::npos ? text.size() : eol) - at, "load_ref_pose_from_file = 0\r");
        }
        at += 1;
    }
    FILE* out = nullptr;
    if (_wfopen_s(&out, dstIni.c_str(), L"wb") || !out) return "could not copy OXRMC config into FH3's sandbox";
    fwrite(text.data(), 1, text.size(), out);
    fclose(out);
    return "copied OXRMC config into FH3's sandbox (restart FH3 for it to load)";
}

static DWORD WINAPI BridgeThread(void*) {
    struct Feed {
        HANDLE src = nullptr, dst = nullptr;
        const uint8_t* srcView = nullptr;
        uint8_t* dstView = nullptr;
        size_t size = 0;
        void CloseSrc() {
            if (srcView) UnmapViewOfFile(srcView);
            if (src) CloseHandle(src);
            src = nullptr; srcView = nullptr; size = 0;
        }
        void CloseDst() {
            if (dstView) UnmapViewOfFile(dstView);
            if (dst) CloseHandle(dst);
            dst = nullptr; dstView = nullptr;
        }
    } feeds[std::size(kFeeds)];
    DWORD cur = 0;
    std::wstring ns;          // FH3's object namespace, "" = not reachable
    ULONGLONG lastOpen = 0;
    for (;;) {
        DWORD want = bridge::pid.load();
        if (want != cur) {
            for (auto& f : feeds) f.CloseDst();
            cur = want;
            ns.clear();
            bridge::state = 0;
            bridge::feeds = 0;
            lastOpen = 0;
            if (cur) {
                HANDLE tok = OpenToken(cur);
                wchar_t path[512];
                ULONG n = 0;
                if (tok && !ContainerSid(tok).empty() && GetAppContainerNamedObjectPath(tok, nullptr, 512, path, &n))
                    ns = path;
                bridge::state = ns.empty() ? -1 : 1;
                if (tok) CloseHandle(tok);
            }
        }
        if (ns.empty()) { Sleep(200); continue; }
        ULONGLONG now = GetTickCount64();
        if (now - lastOpen > 2000) {  // reopen now and then: the motion software may have restarted
            lastOpen = now;
            for (size_t i = 0; i < std::size(kFeeds); ++i) {
                Feed& f = feeds[i];
                f.CloseSrc();
                f.src = OpenFileMappingW(FILE_MAP_READ, FALSE, (std::wstring(L"Local\\") + kFeeds[i].name).c_str());
                if (f.src) f.srcView = (const uint8_t*)MapViewOfFile(f.src, FILE_MAP_READ, 0, 0, 0);
                MEMORY_BASIC_INFORMATION mbi{};
                f.size = f.srcView && VirtualQuery(f.srcView, &mbi, sizeof(mbi)) ? std::min<size_t>(mbi.RegionSize, 4096) : 0;
                if (f.size && !f.dstView) {
                    // Everyone + all app packages, low integrity label so the sandbox may open it.
                    PSECURITY_DESCRIPTOR sd = nullptr;
                    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                            L"D:(A;;GA;;;WD)(A;;GA;;;AC)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sd, nullptr)) {
                        SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
                        f.dst = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 4096,
                                                   (ns + L"\\" + kFeeds[i].name).c_str());
                        LocalFree(sd);
                    }
                    if (f.dst) f.dstView = (uint8_t*)MapViewOfFile(f.dst, FILE_MAP_WRITE, 0, 0, 4096);
                    if (!f.dstView) { f.CloseDst(); bridge::state = -1; }
                }
            }
        }
        unsigned live = 0;
        for (size_t i = 0; i < std::size(kFeeds); ++i) {
            Feed& f = feeds[i];
            if (f.srcView && f.size && f.dstView) {
                memcpy(f.dstView, f.srcView, f.size);
                live |= 1u << i;
            }
        }
        bridge::feeds = live;
        if (bridge::state != -1) bridge::state = live ? 2 : 1;
        Sleep(1);
    }
}

// ---- OXRMC hotkeys ----
// OXRMC reads its shortcuts with GetAsyncKeyState, which sees nothing inside FH3's sandbox, so
// Ctrl+Ins / Ctrl+Del / Ctrl+Shift+HOME etc. do nothing in FH3. OXRMC also takes the same
// actions through its own command block "OXRMC_ActivityInput" (two 64-bit words: trigger,
// confirm; set a trigger bit, OXRMC runs it and sets the confirm bit, then clear the trigger).
// This thread watches the keyboard from outside, with the shortcuts from FH3's OXRMC config and
// OXRMC's own matching rules, and forwards presses while FH3 is the focused window.
namespace keys {
std::atomic<bool> linked{false};   // OXRMC's command block in FH3 is open
std::atomic<int> lastAction{-1};   // last forwarded action (index into kActions)
std::atomic<unsigned> count{0};    // forwarded actions so far
}  // namespace keys

struct OxrmcAction { const char* key; int bit; bool fast; const char* name; };
static const OxrmcAction kActions[] = {
    {"activate", 0, false, "activate/deactivate"},     {"calibrate", 1, false, "calibrate"},
    {"lock_reference_pose", 2, false, "lock reference pose"},
    {"release_reference_pose", 3, false, "release reference pose"},
    {"translation_increase", 4, true, "translation filter +"}, {"translation_decrease", 5, true, "translation filter -"},
    {"rotation_increase", 6, true, "rotation filter +"},       {"rotation_decrease", 7, true, "rotation filter -"},
    {"toggle_stabilizer", 8, true, "stabilizer on/off"},       {"stabilizer_increase", 9, true, "stabilizer +"},
    {"stabilizer_decrease", 10, true, "stabilizer -"},         {"offset_forward", 11, true, "offset forward"},
    {"offset_back", 12, true, "offset back"},                  {"offset_up", 13, true, "offset up"},
    {"offset_down", 14, true, "offset down"},                  {"offset_right", 15, true, "offset right"},
    {"offset_left", 16, true, "offset left"},                  {"rotate_right", 17, true, "rotate right"},
    {"rotate_left", 18, true, "rotate left"},                  {"toggle_overlay", 19, false, "overlay"},
    {"toggle_overlay_passthrough", 20, false, "overlay passthrough"},
    {"toggle_crosshair", 21, false, "crosshair"},              {"toggle_cache", 22, false, "eye cache"},
    {"toggle_pose_modifier", 23, false, "pose modifier"},      {"save_config", 24, false, "save config"},
    {"save_config_app", 25, false, "save app config"},         {"reload_config", 26, false, "reload config"},
    {"toggle_verbose_logging", 27, false, "verbose logging"},  {"toggle_recording", 28, false, "recording"},
    {"log_tracker_pose", 29, false, "log tracker pose"},       {"log_interaction_profile", 30, false, "log interaction profile"},
};
static const struct { const char* n; int vk; } kVk[] = {
    {"BACK", VK_BACK}, {"TAB", VK_TAB}, {"CLR", VK_CLEAR}, {"RETURN", VK_RETURN}, {"SHIFT", VK_SHIFT},
    {"CTRL", VK_CONTROL}, {"ALT", VK_MENU}, {"PAUSE", VK_PAUSE}, {"CAPS", VK_CAPITAL}, {"ESC", VK_ESCAPE},
    {"SPACE", VK_SPACE}, {"PGUP", VK_PRIOR}, {"PGDN", VK_NEXT}, {"END", VK_END}, {"HOME", VK_HOME},
    {"LEFT", VK_LEFT}, {"UP", VK_UP}, {"RIGHT", VK_RIGHT}, {"DOWN", VK_DOWN}, {"SELECT", VK_SELECT},
    {"PRINT", VK_PRINT}, {"PRTSC", VK_SNAPSHOT}, {"EXEC", VK_EXECUTE}, {"INS", VK_INSERT}, {"DEL", VK_DELETE},
    {"HELP", VK_HELP}, {"NUMMULTIPLY", VK_MULTIPLY}, {"NUMADD", VK_ADD}, {"NUMSEPARATOR", VK_SEPARATOR},
    {"NUMSUBTRACT", VK_SUBTRACT}, {"NUMDECIMAL", VK_DECIMAL}, {"NUMDIVIDE", VK_DIVIDE}, {"NUMLOCK", VK_NUMLOCK},
    {"SCROLL", VK_SCROLL}, {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT}, {"LCTRL", VK_LCONTROL},
    {"RCTRL", VK_RCONTROL}, {"LALT", VK_LMENU}, {"RALT", VK_RMENU}, {"SEMICOLON", VK_OEM_1},
    {"PLUS", VK_OEM_PLUS}, {"COMMA", VK_OEM_COMMA}, {"MINUS", VK_OEM_MINUS}, {"PERIOD", VK_OEM_PERIOD},
    {"SLASH", VK_OEM_2}, {"BACKQUOTE", VK_OEM_3}, {"OPENBRACKET", VK_OEM_4}, {"BACKSLASH", VK_OEM_5},
    {"CLOSEBRACKET", VK_OEM_6}, {"QUOTE", VK_OEM_7},
};

static int VkFromName(const std::string& n) {
    if (n.size() == 1 && ((n[0] >= '0' && n[0] <= '9') || (n[0] >= 'A' && n[0] <= 'Z'))) return n[0];
    if (n.size() == 4 && n.rfind("NUM", 0) == 0 && n[3] >= '0' && n[3] <= '9') return VK_NUMPAD0 + (n[3] - '0');
    if (n.size() >= 2 && n.size() <= 3 && n[0] == 'F') {
        int f = atoi(n.c_str() + 1);
        if (f >= 1 && f <= 12) return VK_F1 + f - 1;
    }
    for (auto& k : kVk) if (n == k.n) return k.vk;
    return 0;
}

struct Shortcut { std::vector<int> keys, exclude; bool down = false; ULONGLONG since = 0; };

// The shortcuts as OXRMC sees them: a key from FH3's sandbox config (where OXRMC reads it), with
// CTRL/SHIFT/ALT that are not part of it excluded (ALT, the fast modifier, allowed for repeating ones).
static void LoadShortcuts(const std::wstring& ini, Shortcut (&sc)[std::size(kActions)]) {
    for (size_t i = 0; i < std::size(kActions); ++i) {
        sc[i] = Shortcut{};
        wchar_t key[64], v[128];
        MultiByteToWideChar(CP_ACP, 0, kActions[i].key, -1, key, 64);
        GetPrivateProfileStringW(L"shortcuts", key, L"", v, 128, ini.c_str());
        std::string s;
        for (wchar_t* c = v; *c; ++c) s += char(*c < 128 ? *c : '?');
        if (s.empty() || s == "NONE") continue;
        bool bad = false;
        for (size_t at = 0; at <= s.size();) {
            size_t plus = s.find('+', at);
            std::string name = s.substr(at, plus == std::string::npos ? std::string::npos : plus - at);
            int vk = VkFromName(name);
            if (!vk) bad = true;
            else sc[i].keys.push_back(vk);
            if (plus == std::string::npos) break;
            at = plus + 1;
        }
        if (bad) { sc[i].keys.clear(); continue; }
        for (int m : {VK_CONTROL, VK_SHIFT, VK_MENU}) {
            if (std::find(sc[i].keys.begin(), sc[i].keys.end(), m) != sc[i].keys.end()) continue;
            if (m == VK_MENU && kActions[i].fast) continue;
            sc[i].exclude.push_back(m);
        }
    }
}

// Is a window of process pid in front? A UWP game's window belongs to ApplicationFrameHost, with
// the game's CoreWindow as a child.
static bool InFront(DWORD pid) {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD p = 0;
    GetWindowThreadProcessId(fg, &p);
    if (p == pid) return true;
    struct Find { DWORD pid; bool hit; } f{pid, false};
    EnumChildWindows(fg, [](HWND h, LPARAM lp) -> BOOL {
        auto* f = (Find*)lp;
        DWORD p = 0;
        GetWindowThreadProcessId(h, &p);
        if (p == f->pid) { f->hit = true; return FALSE; }
        return TRUE;
    }, (LPARAM)&f);
    return f.hit;
}

static DWORD WINAPI KeysThread(void*) {
    DWORD cur = 0;
    std::wstring nsPath, ini;
    HANDLE map = nullptr;
    volatile LONG64* cmd = nullptr;   // [0] trigger, [1] confirm
    Shortcut sc[std::size(kActions)];
    FILETIME iniTime{};
    ULONGLONG lastTry = 0, sentAt[64] = {};
    uint64_t pending = 0;             // trigger bits set, waiting for OXRMC's confirm
    for (;;) {
        Sleep(5);
        DWORD want = bridge::pid.load();
        ULONGLONG now = GetTickCount64();
        if (want != cur) {
            if (cmd) UnmapViewOfFile((void*)cmd);
            if (map) CloseHandle(map);
            map = nullptr; cmd = nullptr; pending = 0;
            keys::linked = false;
            cur = want;
            nsPath.clear(); ini.clear();
            iniTime = FILETIME{};
            if (cur) {
                HANDLE tok = OpenToken(cur);
                wchar_t path[512];
                ULONG n = 0;
                std::wstring sid = tok ? ContainerSid(tok) : L"";
                if (!sid.empty() && GetAppContainerNamedObjectPath(tok, nullptr, 512, path, &n)) nsPath = path;
                PWSTR acDir = nullptr;
                if (!sid.empty() && SUCCEEDED(GetAppContainerFolderPath(sid.c_str(), &acDir))) {
                    ini = std::wstring(acDir) + L"\\OpenXR-MotionCompensation\\OpenXR-MotionCompensation.ini";
                    CoTaskMemFree(acDir);
                }
                if (tok) CloseHandle(tok);
            }
        }
        if (!cur || nsPath.empty()) continue;
        if (!cmd && now - lastTry > 2000) {   // OXRMC makes the block when FH3's VR session starts
            lastTry = now;
            map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, (nsPath + L"\\OXRMC_ActivityInput").c_str());
            if (map) cmd = (volatile LONG64*)MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 16);
            if (map && !cmd) { CloseHandle(map); map = nullptr; }
            keys::linked = cmd != nullptr;
        }
        if (!cmd) continue;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!ini.empty() && GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &fa) &&
            CompareFileTime(&fa.ftLastWriteTime, &iniTime)) {   // (re)load when the config changes
            iniTime = fa.ftLastWriteTime;
            LoadShortcuts(ini, sc);
        }
        // finish handshakes: OXRMC confirmed -> clear our trigger; give up after 2 s
        uint64_t confirm = (uint64_t)cmd[1];
        for (int b = 0; b < 64; ++b) {
            uint64_t m = 1ull << b;
            if ((pending & m) && ((confirm & m) || now - sentAt[b] > 2000)) {
                InterlockedAnd64(&cmd[0], ~(LONG64)m);
                pending &= ~m;
            }
        }
        bool front = InFront(cur);
        for (size_t i = 0; i < std::size(kActions); ++i) {
            Shortcut& s = sc[i];
            if (s.keys.empty()) continue;
            bool pressed = front;
            for (int vk : s.keys) pressed = pressed && GetAsyncKeyState(vk) < 0;
            for (int vk : s.exclude) pressed = pressed && GetAsyncKeyState(vk) >= 0;
            bool fire = false;
            if (pressed && !s.down) { fire = true; s.since = now; }
            else if (pressed && kActions[i].fast && now - s.since > 300) { fire = true; s.since = now; }   // OXRMC's repeat
            s.down = pressed;
            if (!fire) continue;
            uint64_t m = 1ull << kActions[i].bit;
            if ((pending & m) || ((uint64_t)cmd[1] & m)) continue;   // previous press not finished yet
            InterlockedOr64(&cmd[0], (LONG64)m);
            pending |= m;
            sentAt[kActions[i].bit] = now;
            keys::lastAction = int(i);
            ++keys::count;
        }
    }
}

// ---- settle gate ----
// World switches, loads and festival cut-ins bring the car back frozen, drop it,
// teleport it and set its speed instantly; each sends a hard spike to motion rigs
// (12 g measured on a world switch). After any trigger, IsRaceOn stays 0 until the
// car has sat on its wheels with calm acceleration for kCalm seconds.
// Triggers: a different car object, a position jump no car could drive in one tick,
// or speed set from/to a standstill in one tick. Mirrors dyn.Settle in Python.
struct Settle {
    static constexpr double kCalm = 1.0;
    static constexpr float kAccelMax = 15.0f;   // m/s^2, about 1.5 g
    static constexpr float kTeleport = 10.0f;   // metres beyond what the speed explains
    static constexpr float kStill = 0.05f, kSnap = 1.5f;  // m/s

    uint64_t car = 0;
    bool havePos = false;
    float pos[3]{}, speed = 0.0f;
    double tPrev = 0.0, calmSince = -1.0;  // < 0 = settled
    const char* why = "";

    bool Update(double t, uint64_t c, const Packet& p) {
        const char* trig = nullptr;
        if (c != car) trig = "new car";
        else if (havePos) {
            double dt = std::max(t - tPrev, 1e-3);
            float dx = p.Pos[0] - pos[0], dy = p.Pos[1] - pos[1], dz = p.Pos[2] - pos[2];
            float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d > kTeleport + 2.0 * std::max(p.Speed, speed) * dt) trig = "teleport";
            else if (std::min(p.Speed, speed) < kStill && std::fabs(p.Speed - speed) > kSnap) trig = "speed set by game";
        }
        car = c; havePos = true; speed = p.Speed; tPrev = t;
        memcpy(pos, p.Pos, sizeof(pos));
        if (trig) { calmSince = t; why = trig; return true; }
        if (calmSince < 0) return false;
        float a = std::sqrt(p.Accel[0] * p.Accel[0] + p.Accel[1] * p.Accel[1] + p.Accel[2] * p.Accel[2]);
        bool loaded = true;
        for (float s : p.SuspNorm) loaded = loaded && s > 0.01f;
        if (a > kAccelMax || !loaded) calmSince = t;
        else if (t - calmSince >= kCalm) { calmSince = -1.0; return false; }
        return true;
    }
    void Reset() { car = 0; havePos = false; }
};

// ---- console panel ----
// In a real console the window shows a live panel redrawn in place (VT sequences).
// Redirected output falls back to plain timestamped status lines.
namespace ui {
bool vt = false;
std::string state = "starting";
std::deque<std::string> log;
Packet last{};
bool haveCar = false;
uint64_t sent = 0;
double rate = 0.0;
}  // namespace ui

static void Status(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    if (ui::state == buf) return;
    ui::state = buf;
    SYSTEMTIME t; GetLocalTime(&t);
    char line[300];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d] %s", t.wHour, t.wMinute, t.wSecond, buf);
    if (ui::vt) {
        ui::log.push_back(line);
        while (ui::log.size() > 6) ui::log.pop_front();
    } else if (g_screen) {   // (screen=0: nothing at all)
        printf("%s\n", line);
        fflush(stdout);
    }
}

static void Ln(std::string& o, const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    o += buf;
    o += "\x1b[K\n";  // clear the rest of the line
}

static std::string Bar(float f, int w) {
    if (!(f > 0.0f)) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    int n = int(f * w + 0.5f);
    return "[" + std::string(n, '#') + std::string(w - n, '-') + "]";
}

static std::string LapTime(float s) {
    if (!(s > 0.0f)) return "--:--.---";
    int ms = int(s * 1000.0f + 0.5f);
    char b[32];
    snprintf(b, sizeof(b), "%d:%02d.%03d", ms / 60000, ms / 1000 % 60, ms % 1000);
    return b;
}

static void Draw(const std::vector<Dest>& dests) {
    if (!ui::vt) return;
    const Packet& p = ui::last;
    std::string o = "\x1b[H";
    Ln(o, " FH3 Data Out %s - Forza Horizon 3 telemetry (Horizon Data Out, 324 bytes)", kVersion);
    std::string to;
    for (auto& d : dests) to += (to.empty() ? "" : ", ") + d.text;
    Ln(o, " Sending to %s    Ctrl+C to stop", to.c_str());
    Ln(o, "");
    Ln(o, " Status    %s", ui::state.c_str());
    Ln(o, " Packets   %llu sent, %.1f per second, IsRaceOn %d", (unsigned long long)ui::sent, ui::rate, p.IsRaceOn);
    static const char* bs[] = {"failed to reach FH3's sandbox", "off", "no rig feed yet", "mirroring the rig feed into FH3"};
    int b = bridge::state.load();
    if (b) {
        std::string which;
        for (size_t i = 0; i < std::size(kFeeds); ++i)
            if (bridge::feeds.load() & (1u << i)) which += std::string(which.empty() ? " (" : ", ") + kFeeds[i].label;
        if (!which.empty()) which += ")";
        Ln(o, " VR comp   %s%s%s", bs[b + 1], which.c_str(), keys::linked ? ", OXRMC hotkeys work in FH3" : "");
    }
    Ln(o, "");
    if (ui::haveCar) {
        static const char* dt[] = {"FWD", "RWD", "AWD"};
        Ln(o, " Car       id %d   %s   %d cylinders", p.CarOrdinal,
           p.Drivetrain >= 0 && p.Drivetrain <= 2 ? dt[p.Drivetrain] : "?", p.Cylinders);
        Ln(o, " Speed     %6.1f km/h  %6.1f mph     Gear %s", p.Speed * 3.6f, p.Speed * 2.23694f,
           p.Gear == 0 ? "R" : std::to_string(p.Gear).c_str());
        float rf = p.EngineMaxRpm > 0 ? p.CurrentEngineRpm / p.EngineMaxRpm : 0.0f;
        Ln(o, " Engine    %5.0f / %5.0f rpm %s  idle %.0f", p.CurrentEngineRpm, p.EngineMaxRpm,
           Bar(rf, 20).c_str(), p.EngineIdleRpm);
        Ln(o, " Power     %4.0f hp   Torque %4.0f N-m   Boost %5.1f psi   Fuel %3.0f%%",
           p.Power / 745.7f, p.Torque, p.Boost, p.Fuel * 100.0f);
        Ln(o, " Throttle  %s %3d%%    Brake  %s %3d%%", Bar(p.Throttle / 255.0f, 10).c_str(),
           p.Throttle * 100 / 255, Bar(p.Brake / 255.0f, 10).c_str(), p.Brake * 100 / 255);
        Ln(o, " Clutch    %3d%%   Handbrake %3d%%   Steer %+4d", p.Clutch * 100 / 255,
           p.HandBrake * 100 / 255, p.Steer);
        Ln(o, " G-force   lateral %+5.2f   long %+5.2f   vertical %+5.2f", p.Accel[0] / 9.81f,
           p.Accel[2] / 9.81f, p.Accel[1] / 9.81f);
        Ln(o, "");
        Ln(o, "           %8s %8s %8s %8s", "FL", "FR", "RL", "RR");
        Ln(o, " Tire F    %8.0f %8.0f %8.0f %8.0f", p.TireTemp[0], p.TireTemp[1], p.TireTemp[2], p.TireTemp[3]);
        Ln(o, " Susp      %8.2f %8.2f %8.2f %8.2f", p.SuspNorm[0], p.SuspNorm[1], p.SuspNorm[2], p.SuspNorm[3]);
        Ln(o, " Slip      %8.2f %8.2f %8.2f %8.2f", p.SlipComb[0], p.SlipComb[1], p.SlipComb[2], p.SlipComb[3]);
        Ln(o, " Rumble    %8.2f %8.2f %8.2f %8.2f", p.SurfaceRumble[0], p.SurfaceRumble[1],
           p.SurfaceRumble[2], p.SurfaceRumble[3]);
        Ln(o, "");
        if (p.RacePosition)
            Ln(o, " Race      position %u   lap %u   current %s   last %s   best %s   race %s",
               p.RacePosition, p.LapNumber + 1, LapTime(p.CurrentLap).c_str(), LapTime(p.LastLap).c_str(),
               LapTime(p.BestLap).c_str(), LapTime(p.RaceTime).c_str());
        else
            Ln(o, " Race      free roam");
    } else {
        Ln(o, " No car data yet.");
    }
    Ln(o, "");
    Ln(o, " Log");
    for (auto& l : ui::log) Ln(o, " %s", l.c_str());
    o += "\x1b[J";  // clear anything below
    fwrite(o.data(), 1, o.size(), stdout);
    fflush(stdout);
}

static BOOL WINAPI OnCtrl(DWORD) {
    if (ui::vt) { printf("\x1b[?25h\n"); fflush(stdout); }  // show the cursor again
    return FALSE;
}

int main(int argc, char** argv) {
    std::vector<std::string> destText;
    double rate = 60.0;
    bool oxrmc = true;
    ReadIni(destText, rate, oxrmc);
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--no-oxrmc")) oxrmc = false;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("FH3 Data Out %s\nFH3DataOut [host:port ...] [--rate N] [--no-oxrmc]\n", kVersion);
            printf(""
                   "Sends Forza Horizon Data Out for FH3. Default 127.0.0.1:8888, 60 Hz.\n"
                   "Also feeds OpenXR-MotionCompensation inside FH3's sandbox (VR); --no-oxrmc turns that off.\n"
                   "FH3DataOut.ini beside the exe may hold dest=host:port, rate=N, oxrmc=0, screen=0 and precise=0 lines.\n");
            return 0;
        } else destText.push_back(argv[i]);
    }
    if (destText.empty()) destText.push_back("127.0.0.1:8888");
    if (!(rate >= 1.0 && rate <= 1000.0)) rate = 60.0;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { printf("Winsock failed\n"); return 1; }
    std::vector<Dest> dests;
    for (auto& s : destText) {
        Dest d;
        if (ParseDest(s, d)) dests.push_back(d);
        else printf("bad destination \"%s\" (want host:port)\n", s.c_str());
        if (dests.size() == 8) break;
    }
    if (dests.empty()) return 1;
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { printf("socket failed\n"); return 1; }

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!g_screen) {
        printf("FH3 Data Out %s - sending to %s%s at %.0f packets per second.\n", kVersion, dests[0].text.c_str(), dests.size() > 1 ? " (and more)" : "", rate);
        printf("Screen updates are off (screen=0 in FH3DataOut.ini). Ctrl+C to stop.\n");
    } else if (GetConsoleMode(out, &mode) && SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        ui::vt = true;
        SetConsoleTitleW(L"FH3 Data Out 1.0.0");
        SetConsoleCtrlHandler(OnCtrl, TRUE);
        printf("\x1b[?25l\x1b[2J");  // hide cursor, clear
    } else {
        printf("FH3 Data Out %s - Forza Horizon 3 telemetry, Horizon Data Out format (324 bytes)\n", kVersion);
        for (auto& d : dests) printf("  sending to %s\n", d.text.c_str());
        printf("  %.0f packets per second. Ctrl+C to stop.\n\n", rate);
    }

    if (g_precise) timeBeginPeriod(1);
    if (oxrmc) {
        CloseHandle(CreateThread(nullptr, 0, BridgeThread, nullptr, 0, nullptr));
        CloseHandle(CreateThread(nullptr, 0, KeysThread, nullptr, 0, nullptr));
    }
    unsigned keysSeen = 0;
    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    const double period = 1.0 / rate;
    double next = 0.0;

    Game g;
    Block blk;
    Rumble rumble;
    Settle settle;
    uint64_t iface = 0, car = 0;
    double nextResolve = 0.0, lastDraw = -1.0, rateMark = 0.0;
    uint64_t sentMark = 0;

    for (;;) {
        QueryPerformanceCounter(&now);
        double t = double(now.QuadPart - t0.QuadPart) / freq.QuadPart;
        if (t - rateMark >= 1.0) {
            ui::rate = (ui::sent - sentMark) / (t - rateMark);
            sentMark = ui::sent;
            rateMark = t;
        }
        if (g_screen && t - lastDraw >= 0.1) {  // 10 redraws a second is plenty to read
            lastDraw = t;
            if (unsigned kc = keys::count.load(); kc != keysSeen) {
                keysSeen = kc;
                int a = keys::lastAction.load();
                if (a >= 0) Status("OXRMC: %s", kActions[a].name);
            }
            Draw(dests);
        }
        if (!g.alive()) {
            g.close();
            bridge::pid = 0;
            iface = 0;
            ui::haveCar = false;
            ui::last = Packet{};
            if (!Attach(g)) { Status("waiting for Forza Horizon 3..."); Sleep(1000); continue; }
            if (g.size != kImageSize) {
                Status("unsupported FH3 build (image size 0x%X, want 0x%X)", g.size, kImageSize);
                g.close();
                Sleep(5000);
                continue;
            }
            Status("found Forza Horizon 3 (pid %lu)", g.pid);
            if (oxrmc) {
                if (const char* note = OxrmcConfig(g.pid)) Status("%s", note);
                bridge::pid = g.pid;
            }
        }
        int paused = -1;
        if (!iface || t >= nextResolve) {
            iface = PlayerDynamics(g, &paused, &car);
            nextResolve = t + 0.5;
        } else {
            uint64_t forza = g.u64(g.base + m::kForza);
            uint64_t mgr = forza ? g.u64(forza + m::kGameMgr) : 0;
            uint8_t pb = 0;
            if (mgr && g.read(mgr + m::kPause, &pb, 1)) paused = pb != 0;
        }
        bool ok = iface && g.u64(iface) == g.base + m::kIfaceVt && g.read(iface + m::kSpanLo, blk.buf.data(), blk.buf.size()) && Plausible(blk);
        if (!ok) iface = 0;
        // Like FH4-6 in practice: packets flow the whole time the game runs.
        // IsRaceOn = 1 only while driving; paused keeps the last car data, no car sends zeros.
        Packet p{};
        if (ok) {
            Fill(blk, p);
            rumble.Fill(g, blk, p, t);
            if (car) FillIdentityRace(g, car, p);
        }
        bool settling = false;
        if (ok) settling = settle.Update(t, car, p);
        else settle.Reset();
        p.IsRaceOn = ok && paused == 0 && !settling;
        p.TimestampMS = (uint32_t)(uint64_t)(t * 1000.0);
        for (auto& d : dests)
            sendto(sock, (const char*)&p, sizeof(p), 0, (const sockaddr*)&d.addr, sizeof(d.addr));
        ++ui::sent;
        ui::last = p;
        ui::haveCar = ok;
        if (!ok) Status("no car (menus or loading), sending IsRaceOn 0");
        else if (p.IsRaceOn) Status("driving, sending");
        else if (paused != 0) Status("paused, sending IsRaceOn 0");
        else Status("settling (%s), sending IsRaceOn 0", settle.why);
        if (next == 0.0) next = t;
        next += period;
        QueryPerformanceCounter(&now);
        double left = next - double(now.QuadPart - t0.QuadPart) / freq.QuadPart;
        if (left > 0.002) Sleep(DWORD((left - 0.001) * 1000.0));
        if (left < -0.25) next = 0.0;  // fell far behind (alt-tab, load): resync
        for (; g_precise;) {  // short spin for the last millisecond
            QueryPerformanceCounter(&now);
            if (double(now.QuadPart - t0.QuadPart) / freq.QuadPart >= next || next == 0.0) break;
            YieldProcessor();
        }
    }
}
