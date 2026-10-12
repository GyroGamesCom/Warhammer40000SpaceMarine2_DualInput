/*
 * dinput8.dll proxy for Warhammer 40,000: Space Marine 2 (Swarm engine, x64).
 *
 * "Dual input": the gamepad stays the game's active (focus) device -> gamepad glyphs and the
 * gamepad camera settings -- while mouse motion (Steam Input generates it from gyro) is fed into
 * the camera as native-scale mouse look. The real right stick (Steam right trackpad) above a
 * deadzone always wins.
 *
 * How it works (see analysis\ANALYSIS.md; everything is found by AOB scan, no hardcoded VAs
 * except for logging / diagnostics)
 *  - The exe statically imports dinput8.dll, so this DLL is a plain proxy (exports forwarded to
 *    the system DLL, no COM wrapping). The game polls the mouse through its own DirectInput
 *    device class (absolute axes, deltas computed by the game), so we hook inside the game.
 *  - Three vtable slots in .rdata are patched (each located by scanning .rdata for the absolute
 *    address of the function found by AOB):
 *      inpSYSTEM::Update   frame bracket: snapshot of mouse motion, frame counter, F10, focus log
 *      MouseDevice::Update after the original: while the gamepad has focus, mouse motion
 *                          (+0x15dc/+0x15e0 and channels +0x90..+0x9c) is moved into an accumulator
 *                          and zeroed -> mouse can no longer steal focus or reach the stick model
 *      InputActionRotate::GetRotation  after the original: adds the accumulated motion, scaled like
 *                          the native mouse path, to the two outputs (pitch/yaw, configurable)
 *  - Injection only into the gameplay camera action (identified by its key bindings); the right-stick
 *    override reads the camera action's combined command values (works with Steam Input).
 *  - XInputGetState hook is diagnostics only.
 *  - Any verification failure -> that hook is not installed ("FALLBACK", game stays vanilla).
 *    ROTATE needs INPSYS (frame ids); MOUSE (suppression) needs ROTATE (else motion would be lost).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

/* ---- minimal XInput definitions ---- */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XGAMEPAD;
typedef struct { DWORD dwPacketNumber; XGAMEPAD Gamepad; } XSTATE;
typedef DWORD (WINAPI *PFN_GetState)(DWORD, XSTATE *);

/* hooked game functions: all x64 "fastcall"; extra register args are passed through blindly */
typedef ULONG_PTR (__fastcall *PFN_Upd)(void *, void *, void *, void *);
typedef void (__fastcall *PFN_Rot)(void *, float, float *, float *);

#define EXPECTED_EXE_SIZE 84992040ULL
/* RVAs from the static analysis, used ONLY for logging / diagnostics (guarded by image size) */
#define RVA_MOUSE_ORIG   0x4f11760u   /* mouse type id (original)           */
#define RVA_KB_ORIG      0x4f1172cu   /* keyboard type id (original)        */
#define RVA_XENON        0x4f11728u
#define RVA_DS4          0x4f11764u
#define RVA_GENERIC      0x4f118f8u
#define RVA_STEAM_B0     0x4f11698u   /* Steam input device update needs [0]!=0 and [1]!=0 (?) */
#define RVA_STEAM_B1     0x4f11699u
#define RVA_EXP_GS       0x4dea4f0u
#define RVA_EXP_SLOT_INP 0x364e020u
#define RVA_EXP_SLOT_MSE 0x364e1d8u
#define RVA_EXP_SLOT_ROT 0x35e8328u

/* ---- globals ---- */
static HMODULE g_self, g_real;
static char    g_dir[MAX_PATH], g_ini[MAX_PATH], g_logPath[MAX_PATH], g_oldLogPath[MAX_PATH], g_exePath[MAX_PATH];
static BYTE   *g_exe;
static DWORD   g_imgSize;
static ULONGLONG g_exeFileSize;
static LARGE_INTEGER g_freq;
static BYTE   *g_text, *g_rdata;
static SIZE_T  g_textSize, g_rdataSize;

/* settings */
static volatile LONG g_enabled = 1;
static float g_gyroSens = 1.0f, g_override = 0.27f;
static int   g_cameraMatch = 1, g_trackpadOv = 1, g_invX, g_invY, g_swap, g_padIndex, g_forceFocus, g_logOn = 1, g_diag = 1;

/* ---------------------------------------------------------------- logging */
static CRITICAL_SECTION g_logCs, g_initCs;
static FILE *g_logFile;

static void Log(const char *fmt, ...)
{
    va_list ap;
    SYSTEMTIME st;
    if (!g_logOn || !g_logFile) return;
    EnterCriticalSection(&g_logCs);
    GetLocalTime(&st);
    fprintf(g_logFile, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logFile);
    fflush(g_logFile);
    LeaveCriticalSection(&g_logCs);
}

/* ---------------------------------------------------------------- settings */
static const char k_defaultIni[] =
    "; xinput_mouse.ini - dual input proxy for Warhammer 40,000: Space Marine 2 (gyro/mouse look + gamepad at once)\r\n"
    "[Mouse]\r\n"
    "; 1 = mouse/gyro look is injected into the gamepad camera, 0 = pure pass-through. F10 toggles at runtime (game window focused)\r\n"
    "Enabled=1\r\n"
    "; multiplier on top of the game's own mouse sensitivity (native mouse scale = 1.0)\r\n"
    "GyroSensitivity=1.0\r\n"
    "; 1 = inject only into the gameplay camera action (fingerprint of its bindings); 0 = inject into ALL rotate actions (debug only, breaks menus)\r\n"
    "CameraMatch=1\r\n"
    "; 1 = right stick (Steam right trackpad) above StickOverrideDeadzone discards the gyro (stick read from the camera action's command values)\r\n"
    "TrackpadOverride=1\r\n"
    "; fraction of full stick travel (0..1) above which the real right stick wins\r\n"
    "StickOverrideDeadzone=0.27\r\n"
    "; 1 = flip the horizontal axis\r\n"
    "InvertX=0\r\n"
    "; 1 = flip the vertical axis (on top of the game's own invert-mouse-look option)\r\n"
    "InvertY=0\r\n"
    "; 0 = mouse Y -> first GetRotation output, mouse X -> second; 1 = swapped (use if yaw/pitch are crossed)\r\n"
    "SwapAxes=0\r\n"
    "; XInput user index (diagnostics only; the override no longer uses XInput)\r\n"
    "PadIndex=0\r\n"
    "; 1 = force the game's focus device to the gamepad whenever it switches to mouse (breaks native mouse look!)\r\n"
    "ForceGamepadFocus=0\r\n"
    "; write mixed_input.log next to the DLL\r\n"
    "Log=1\r\n"
    "; extra diagnostics in the log (device list, command values, status lines)\r\n"
    "Diag=1\r\n";

static float GetF(const char *key, const char *def)
{
    char buf[64];
    GetPrivateProfileStringA("Mouse", key, def, buf, sizeof buf, g_ini);
    return (float)atof(buf);
}

static void LoadSettings(void)
{
    if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES) {
        FILE *f = fopen(g_ini, "wb");
        if (f) { fwrite(k_defaultIni, 1, sizeof k_defaultIni - 1, f); fclose(f); }
    }
    g_enabled     = GetPrivateProfileIntA("Mouse", "Enabled", 1, g_ini) != 0;
    g_gyroSens    = GetF("GyroSensitivity", "1.0");
    g_trackpadOv  = GetPrivateProfileIntA("Mouse", "TrackpadOverride", 1, g_ini) != 0;
    g_cameraMatch = GetPrivateProfileIntA("Mouse", "CameraMatch", 1, g_ini) != 0;
    g_override    = GetF("StickOverrideDeadzone", "0.27");
    g_invX        = GetPrivateProfileIntA("Mouse", "InvertX", 0, g_ini) != 0;
    g_invY        = GetPrivateProfileIntA("Mouse", "InvertY", 0, g_ini) != 0;
    g_swap        = GetPrivateProfileIntA("Mouse", "SwapAxes", 0, g_ini) != 0;
    g_padIndex    = GetPrivateProfileIntA("Mouse", "PadIndex", 0, g_ini);
    g_forceFocus  = GetPrivateProfileIntA("Mouse", "ForceGamepadFocus", 0, g_ini) != 0;
    g_logOn       = GetPrivateProfileIntA("Mouse", "Log", 1, g_ini) != 0;
    g_diag        = GetPrivateProfileIntA("Mouse", "Diag", 1, g_ini) != 0;
    if (g_padIndex < 0 || g_padIndex > 3) g_padIndex = 0;
    if (g_override < 0.0f) g_override = 0.0f;
    if (g_override > 1.0f) g_override = 1.0f;
}

/* ---------------------------------------------------------------- safe memory helpers */
static int SafeCopy(void *dst, const void *src, size_t n)
{
    __try { memcpy(dst, src, n); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int InImage(const void *p)
{
    return g_exe && (const BYTE *)p >= g_exe && (const BYTE *)p < g_exe + g_imgSize;
}

/* ---------------------------------------------------------------- resolved game addresses */
static BYTE **g_gsPtr;          /* gsINPUT pointer global: *g_gsPtr = singleton */
static int   *g_pMouseId;       /* mouse type id (gameplay copy) */
static int   *g_pKbId;          /* keyboard type id (gameplay copy) = mouse - 0x40 */
static int   *g_pXenonId;       /* xenon gamepad type id */
static void **g_pXiPtr;         /* XInputGetState function pointer global */

static PFN_Upd g_origInp, g_origMouse;
static PFN_Rot g_origRot;
static volatile LONG g_hkInp, g_hkRot, g_hkMouse, g_hooksDone;
static PFN_GetState g_origGetState;
static volatile LONG g_xiHooked;

/* ---------------------------------------------------------------- state / statistics */
static volatile float g_pendDx, g_pendDy;           /* accumulated since last frame bracket */
static volatile float g_frameDx, g_frameDy;         /* snapshot for the current frame */
static volatile LONG  g_frameId;
static volatile LONG  g_lastFocus = -1;
static volatile LONG64 s_switches, s_mouseCalls, s_suppressed, s_rotCalls, s_injected, s_override, s_exc, s_gsNull;
static volatile LONG  s_rotThisFrame, s_rotMaxFrame, s_injThisFrame, s_injMaxFrame;
static double s_sumDx, s_sumDy;
static volatile float s_dt, s_beforeA, s_beforeB, s_afterA, s_afterB, s_k, s_kSens, s_kScale, s_kLook;
static volatile LONG  s_invByte = -1;
static volatile LONG  g_realRX, g_realRY;           /* XInput right stick (diagnostics only; Steam Input pads are invisible to XInput) */
static volatile LONG  s_warnSens, s_warnScale, s_warnLook;
static volatile float s_stick;                      /* last stick magnitude from camera command values */

#define MAXINST 32
typedef struct {
    void *p, *desc;
    int   match;
    LONG  frame;
    LONG64 inj;
    int   over;                                     /* override hysteresis state */
} INST;
static INST g_inst[MAXINST];
static volatile LONG g_instCount;
static volatile LONG s_matched;
static int g_xenonFallback = 0x372;

/* ---------------------------------------------------------------- type ids / names */
typedef struct { const char *name; int *addr; } IDENT;
static IDENT g_ids[8];
static int   g_nIds;

static void AddId(const char *name, int *addr)
{
    if (addr && InImage(addr) && g_nIds < 8) { g_ids[g_nIds].name = name; g_ids[g_nIds].addr = addr; g_nIds++; }
}

static int ReadId(const int *a)
{
    int v = 0;
    if (!SafeCopy(&v, a, 4)) return 0;
    return v;
}

static const char *IdName(int id)
{
    int i;
    if (!id) return "none";
    for (i = 0; i < g_nIds; i++) if (ReadId(g_ids[i].addr) == id) return g_ids[i].name;
    return "other";
}

/* gamepad focus: a non-zero focus id that is neither mouse nor keyboard */
static int PadFocus(int *pfocus)
{
    int f = 0, m = 0, k = 0;
    __try {
        BYTE *gs = *g_gsPtr;
        if (!gs) { s_gsNull++; return 0; }
        f = *(volatile int *)gs;
        m = *(volatile int *)g_pMouseId;
        k = *(volatile int *)g_pKbId;
    } __except (EXCEPTION_EXECUTE_HANDLER) { s_exc++; return 0; }
    if (pfocus) *pfocus = f;
    return f != 0 && f != m && f != k;
}

/* ---------------------------------------------------------------- hotkey */
static void HandleHotkey(void)
{
    static int prev;
    int down;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (down && !prev) {
        LONG nv = !g_enabled;
        InterlockedExchange(&g_enabled, nv);
        g_pendDx = g_pendDy = 0;
        Log("F10: Enabled=%ld", nv);
    }
    prev = down;
}

/* ---------------------------------------------------------------- camera action fingerprint */
static BYTE *CmdPtr(BYTE *t, int i)
{
    BYTE *c = NULL;
    __try { c = *(BYTE **)(t + 0x40 + 8 * i); } __except (EXCEPTION_EXECUTE_HANDLER) { c = NULL; }
    return c;
}

/* true if some entry of cmd contains devId in its device list and key in its key list */
static int HasBind(BYTE *cmd, int devId, int key)
{
    int n, j, k;
    BYTE *entries;
    if (!cmd) return 0;
    __try {
        n = *(int *)(cmd + 0x28);
        entries = *(BYTE **)(cmd + 0x20);
        if (n <= 0 || n > 64 || !entries) return 0;
        for (j = 0; j < n; j++) {
            BYTE *e = entries + 0x28 * j;
            int *devs = *(int **)(e + 0x08), nDev = *(int *)(e + 0x10);
            int *keys = *(int **)(e + 0x18), nKeys = *(int *)(e + 0x20);
            int hasDev = 0;
            if (!devs || !keys || nDev <= 0 || nDev > 32 || nKeys <= 0 || nKeys > 32) continue;
            for (k = 0; k < nDev; k++) if (devs[k] == devId) { hasDev = 1; break; }
            if (!hasDev) continue;
            for (k = 0; k < nKeys; k++) if (keys[k] == key) return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}

/* compact dump of all (dev:key) pairs of a command into buf */
static void DescribeCmd(BYTE *cmd, char *buf, size_t cap)
{
    int n, j, k, kk;
    BYTE *entries;
    size_t len = 0;
    buf[0] = 0;
    if (!cmd) { strcpy(buf, "null"); return; }
    __try {
        n = *(int *)(cmd + 0x28);
        entries = *(BYTE **)(cmd + 0x20);
        if (n <= 0 || n > 64 || !entries) { _snprintf(buf, cap, "n=%d", n); buf[cap - 1] = 0; return; }
        for (j = 0; j < n; j++) {
            BYTE *e = entries + 0x28 * j;
            int *devs = *(int **)(e + 0x08), nDev = *(int *)(e + 0x10);
            int *keys = *(int **)(e + 0x18), nKeys = *(int *)(e + 0x20);
            if (!devs || !keys || nDev <= 0 || nDev > 32 || nKeys <= 0 || nKeys > 32) continue;
            for (k = 0; k < nDev; k++)
                for (kk = 0; kk < nKeys; kk++) {
                    int w;
                    if (len + 24 >= cap) goto done;
                    w = _snprintf(buf + len, cap - len, "%x:%d ", (unsigned)devs[k], keys[kk]);
                    if (w < 0) goto done;
                    len += (size_t)w;
                }
        }
    done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    if (len && len < cap) buf[len - 1] = 0; else if (len == 0) buf[0] = 0;
}

static int ClassifyInst(BYTE *t, BYTE *desc, float *psens)
{
    int xenon = g_xenonFallback, mouse = 0, m;
    float sens = 0.0f;
    BYTE *c[4];
    int i;
    if (g_pXenonId) { int v = ReadId(g_pXenonId); if (v) xenon = v; }
    mouse = g_pMouseId ? ReadId(g_pMouseId) : 0;
    for (i = 0; i < 4; i++) c[i] = CmdPtr(t, i);
    if (desc) { __try { sens = *(volatile float *)(desc + 0x130); } __except (EXCEPTION_EXECUTE_HANDLER) { sens = 0.0f; } }
    *psens = sens;
    m = HasBind(c[0], xenon, 38) && HasBind(c[1], xenon, 39) && HasBind(c[2], xenon, 37) && HasBind(c[3], xenon, 36)
        && mouse && HasBind(c[0], mouse, 35) && sens > 0.0f;
    if (g_diag) {
        char d[4][160];
        for (i = 0; i < 4; i++) DescribeCmd(c[i], d[i], sizeof d[i]);
        Log("INSTANCE %p desc=%p sens=%.3f matched=%d | fwd[%s] back[%s] left[%s] right[%s]", (void *)t, (void *)desc, (double)sens, m, d[0], d[1], d[2], d[3]);
    } else Log("INSTANCE %p desc=%p sens=%.3f matched=%d", (void *)t, (void *)desc, (double)sens, m);
    return m;
}

/* instance table lookup; (re)classifies when new or when its desc pointer changed */
static INST *GetInst(BYTE *t)
{
    int i, n = g_instCount;
    BYTE *desc = NULL;
    INST *in = NULL;
    __try { desc = *(BYTE **)(t + 0x38); } __except (EXCEPTION_EXECUTE_HANDLER) { desc = NULL; }
    for (i = 0; i < n; i++) if (g_inst[i].p == t) { in = &g_inst[i]; break; }
    if (!in) {
        if (n >= MAXINST) return NULL;
        in = &g_inst[n];
        memset(in, 0, sizeof *in);
        in->p = t; in->desc = (void *)(ULONG_PTR)1;       /* force classification */
        in->frame = -1;
        InterlockedExchange(&g_instCount, n + 1);
    }
    if (in->desc != (void *)desc) {
        float sens;
        int was = in->match, m = ClassifyInst(t, desc, &sens);
        in->desc = desc; in->match = m;
        if (m != was) InterlockedAdd(&s_matched, m ? 1 : -1);
    }
    return in;
}

/* ---------------------------------------------------------------- hooks */
static DWORD WINAPI HookGetState(DWORD user, XSTATE *st)
{
    DWORD r = g_origGetState ? g_origGetState(user, st) : ERROR_DEVICE_NOT_CONNECTED;
    if ((int)user == g_padIndex && r == ERROR_SUCCESS && st) {
        g_realRX = st->Gamepad.sThumbRX;
        g_realRY = st->Gamepad.sThumbRY;
    }
    return r;
}

/* inpSYSTEM::Update -- once per frame; runs the device Updates (so our mouse hook has run by the time orig returns) */
static ULONG_PTR __fastcall HookInpSysUpdate(void *a, void *b, void *c, void *d)
{
    ULONG_PTR r;
    int focus = 0;
    if (g_forceFocus && g_enabled) {
        __try {
            BYTE *gs = *g_gsPtr;
            int f, x = g_pXenonId ? *(volatile int *)g_pXenonId : 0;
            if (gs && x) {
                f = *(volatile int *)gs;
                if (f && f == *(volatile int *)g_pMouseId) { *(volatile int *)gs = x; gs[4] = 1; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { s_exc++; }
    }
    r = g_origInp(a, b, c, d);
    HandleHotkey();
    /* focus tracking (log every change) */
    __try {
        BYTE *gs = *g_gsPtr;
        if (gs) focus = *(volatile int *)gs;
    } __except (EXCEPTION_EXECUTE_HANDLER) { s_exc++; }
    if (focus != g_lastFocus) {
        LONG old = InterlockedExchange(&g_lastFocus, focus);
        s_switches++;
        if (s_switches < 500)
            Log("FOCUS %s(0x%x) -> %s(0x%x)  frame=%ld", old == -1 ? "init" : IdName(old), (unsigned)old, IdName(focus), (unsigned)focus, g_frameId);
    }
    /* frame bracket: motion collected by the mouse hook becomes this frame's motion */
    g_frameDx = g_pendDx; g_frameDy = g_pendDy;
    g_pendDx = 0; g_pendDy = 0;
    if (s_rotThisFrame > s_rotMaxFrame) s_rotMaxFrame = s_rotThisFrame;
    if (s_injThisFrame > s_injMaxFrame) s_injMaxFrame = s_injThisFrame;
    s_rotThisFrame = 0; s_injThisFrame = 0;
    InterlockedIncrement(&g_frameId);
    return r;
}

/* mouse device Update: while the gamepad has focus take the motion away from the game's input model */
static ULONG_PTR __fastcall HookMouseUpdate(void *dev, void *b, void *c, void *d)
{
    ULONG_PTR r = g_origMouse(dev, b, c, d);
    InterlockedIncrement64(&s_mouseCalls);
    if (g_enabled && dev && PadFocus(NULL)) {
        __try {
            BYTE *p = (BYTE *)dev;
            float dx = *(volatile float *)(p + 0x15dc), dy = *(volatile float *)(p + 0x15e0);
            if (dx != 0.0f || dy != 0.0f) {
                g_pendDx += dx; g_pendDy += dy;
                s_sumDx += dx; s_sumDy += dy;
                s_suppressed++;
            }
            *(volatile float *)(p + 0x15dc) = 0.0f;
            *(volatile float *)(p + 0x15e0) = 0.0f;
            *(volatile float *)(p + 0x90) = 0.0f;       /* X+, X-, Y+, Y- movement channels; wheel (+a0/+a4) and buttons stay */
            *(volatile float *)(p + 0x94) = 0.0f;
            *(volatile float *)(p + 0x98) = 0.0f;
            *(volatile float *)(p + 0x9c) = 0.0f;
        } __except (EXCEPTION_EXECUTE_HANDLER) { s_exc++; }
    }
    return r;
}

static float Sane(float v, volatile LONG *warned, const char *what)
{
    if (v > 0.0001f && v < 1000.0f) return v;       /* NaN fails both compares */
    if (!InterlockedExchange(warned, 1)) Log("WARNING: %s=%g outside (0.0001,1000), using 1.0", what, (double)v);
    return 1.0f;
}

/* stick magnitude from the camera action's combined command values (fwd, back, left, right) */
static int ReadStick(BYTE *t, float *mag)
{
    float v[4], a, b, m;
    int i;
    for (i = 0; i < 4; i++) {
        BYTE *cmd = CmdPtr(t, i);
        if (!cmd) return 0;
        v[i] = *(volatile float *)(cmd + 8);
    }
    a = v[0] - v[1]; b = v[2] - v[3];
    m = sqrtf(a * a + b * b);
    if (!(m >= 0.0f && m < 1000.0f)) m = 0.0f;         /* NaN guard */
    *mag = m;
    return 1;
}

/* InputActionRotate::GetRotation(this, dt, outA, outB) */
static void __fastcall HookGetRotation(void *self, float dt, float *outA, float *outB)
{
    g_origRot(self, dt, outA, outB);
    InterlockedIncrement64(&s_rotCalls);
    s_rotThisFrame++;
    s_dt = dt;
    if (!self || !outA || !outB) return;
    __try {
        BYTE *t = (BYTE *)self, *gs;
        INST *in = GetInst(t);
        int focus;
        float a0 = *outA, b0 = *outB;
        if (!in) return;
        if (g_cameraMatch && !in->match) return;
        if (in->match) {
            float stick = 0.0f;
            if (ReadStick(t, &stick)) {
                s_stick = stick;
                if (in->over) { if (stick < g_override * 0.8f) in->over = 0; }
                else if (stick > g_override) in->over = 1;
            }
        }
        s_beforeA = a0; s_beforeB = b0; s_afterA = a0; s_afterB = b0;
        if (!g_enabled || !t[0x7c]) return;
        if (!PadFocus(&focus)) return;
        if (in->frame == g_frameId) return;              /* this instance already got this frame's motion */
        in->frame = g_frameId;
        {
            float dx = g_frameDx, dy = g_frameDy;
            float sens, scale, look, k, pitch, yaw;
            int inv;
            gs = *g_gsPtr;
            if (!gs) return;
            if (g_trackpadOv && in->over) { s_override++; return; }
            if (dx == 0.0f && dy == 0.0f) return;
            {
                BYTE *ms = *(BYTE **)(t + 0x38);
                sens = ms ? *(volatile float *)(ms + 0x130) : 1.0f;
            }
            scale = *(volatile float *)(t + 0x78);
            look  = *(volatile float *)(gs + 0xe4);
            inv   = *(volatile BYTE *)(gs + 0xe0);
            sens = Sane(sens, &s_warnSens, "mouseSens"); scale = Sane(scale, &s_warnScale, "thisScale"); look = Sane(look, &s_warnLook, "lookSens");
            k = sens * 0.05f * scale * look * g_gyroSens;
            s_k = k; s_kSens = sens; s_kScale = scale; s_kLook = look; s_invByte = inv;
            /* native mouse path: A = back - fwd (mouse Y+ is bound to back), B = left - right (mouse X+ is bound to right) */
            pitch = k * dy;
            yaw   = -k * dx;
            if (inv) pitch = -pitch;                     /* native mouse path negates A with invert-look */
            if (g_invY) pitch = -pitch;
            if (g_invX) yaw = -yaw;
            if (g_swap) { *outA += yaw; *outB += pitch; } else { *outA += pitch; *outB += yaw; }
            s_afterA = *outA; s_afterB = *outB;
            s_injected++;
            in->inj++;
            s_injThisFrame++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { s_exc++; }
}

/* ---------------------------------------------------------------- AOB scanning */
typedef struct { const char *name; const char *sig; BYTE b[128], m[128]; int n; BYTE *hit; int cnt; int optional; } SIG;

static void ParseSig(SIG *s)
{
    const char *p = s->sig;
    s->n = 0;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (p[0] == '?') { s->b[s->n] = 0; s->m[s->n] = 0; p += 2; }
        else { s->b[s->n] = (BYTE)strtoul(p, NULL, 16); s->m[s->n] = 1; p += 2; }
        s->n++;
    }
}

static void ScanSig(SIG *s)
{
    BYTE *p, *end;
    s->hit = NULL; s->cnt = 0;
    if (!g_text || !s->n) return;
    p = g_text; end = g_text + g_textSize - s->n;
    __try {
        while (p <= end) {
            int i;
            p = (BYTE *)memchr(p, s->b[0], (size_t)(end - p) + 1);
            if (!p) break;
            for (i = 1; i < s->n; i++) if (s->m[i] && p[i] != s->b[i]) break;
            if (i == s->n) { if (!s->cnt) s->hit = p; s->cnt++; if (s->cnt > 1) break; }
            p++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { s->cnt = -1; }
}

static SIG g_sig[] = {
    { "ROTATE_GetRotation", "48 8B C4 53 56 57 41 54 41 57 48 81 EC D0 00 00 00 0F 29 70 C8 4D 8B F9 0F 29 78 B8 4D 8B E0 44 0F 29 40 A8 48 8B F9 44 0F 29 48 98 44 0F 29 50 88 44 0F 28 D1 44 0F 29 74 24 40 44 0F 29 7C 24 30 48 89 68 10 4C 89 70 18 44 0F 29 64 24 60 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 45 0F 57 C9 41 0F 94 C6 80 7F 7C 00" },
    { "ROTATE_short", "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 45 0F 57 C9 41 0F 94 C6 80 7F 7C 00" },
    { "MOUSE_Update", "48 8B C4 53 48 81 EC 20 03 00 00 48 8B D9 48 8B 89 78 11 00 00 48 85 C9 0F 84 ?? ?? ?? ?? 48 89 78 10 4C 8D 83 EC 15 00 00 0F 29 70 E8 BA 14 00 00 00 0F 29 78 D8 48 8B 01 FF 50 48" },
    { "INPSYS_Update", "48 89 5C 24 18 48 89 6C 24 20 56 48 83 EC 20 80 79 20 00 48 8B D9 74 05 E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 74 37 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 83 38 00 74 24" },
    { "XINPUT_Update", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B D9 33 D2 48 83 C1 10 41 B8 50 05 00 00 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 8B BB 3C 12 00 00" },
    { "XINPUT_ctor", "40 53 48 83 EC 20 48 8D 15 ?? ?? ?? ?? 48 8B D9 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? C7 83 40 12 00 00 00 00 00 00 48 89 03", .optional = 1 },
    { "ISGAMEPADFOCUS", "40 53 48 83 EC 40 48 8B D9 C6 44 24 50 00 48 8D 44 24 50 48 C7 44 24 28 00 00 00 00 0F 57 C0 48 89 44 24 58 48 8D 4C 24 58 F3 0F 7F 44 24 30 E8", .optional = 1 },
    { "GSINPUT_FrameUpdate", "48 8B C4 55 53 41 54 48 8B EC 48 83 EC 60 48 89 70 08 45 33 E4 48 89 78 10 48 8B 3D ?? ?? ?? ?? 4C 89 70 E0", .optional = 1 },
};
enum { S_ROT, S_ROTS, S_MOUSE, S_INP, S_XIU, S_XCTOR, S_ISGP, S_GSF, S_COUNT };

static BYTE *Rel(BYTE *m, int disp, int next)       /* RIP-relative operand: match + next + int32@(match+disp) */
{
    int d;
    memcpy(&d, m + disp, 4);
    return m + next + d;
}

/* parse PE section headers for .text / .rdata */
static int FindSections(void)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int i;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        char nm[9];
        memcpy(nm, sec->Name, 8); nm[8] = 0;
        Log("section %-8s RVA=0x%08lx VSize=0x%08lx RawSize=0x%08lx chars=0x%08lx", nm, sec->VirtualAddress, sec->Misc.VirtualSize, sec->SizeOfRawData, sec->Characteristics);
        if (!strcmp(nm, ".text")) { g_text = g_exe + sec->VirtualAddress; g_textSize = sec->Misc.VirtualSize; }
        else if (!strcmp(nm, ".rdata")) { g_rdata = g_exe + sec->VirtualAddress; g_rdataSize = sec->Misc.VirtualSize; }
    }
    return g_text && g_rdata;
}

/* find the single 8-byte-aligned occurrence of an absolute address in .rdata; returns count, *slot = first */
static int FindSlot(void *fn, BYTE **slot)
{
    int cnt = 0;
    ULONG_PTR *p = (ULONG_PTR *)g_rdata, *end = (ULONG_PTR *)(g_rdata + g_rdataSize);
    *slot = NULL;
    __try {
        for (; p < end; p++) if (*p == (ULONG_PTR)fn) { if (!cnt) *slot = (BYTE *)p; cnt++; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return cnt;
}

static int PatchSlot(BYTE *slot, void *newv, void *expectOld)
{
    DWORD old;
    if (*(void *volatile *)slot != expectOld) return 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return 0;
    InterlockedExchangePointer((PVOID volatile *)slot, newv);
    VirtualProtect(slot, 8, old, &old);
    return 1;
}

/* ---------------------------------------------------------------- diagnostics */
static const char *VtName(DWORD rva)
{
    switch (rva) {
    case 0x364e1b8: return "mouse";
    case 0x364db40: return "xinput(xenon)";
    case 0x364d728: return "hid-ds4/5 (a)";
    case 0x364d970: return "hid-ds4/5 (b)";
    case 0x364e000: return "inpSYSTEM";
    }
    return "?";
}

static int g_lastDevCount = -2;

static int DevCount(void)
{
    int cnt = -1;
    __try {
        BYTE *gs = *g_gsPtr, *sys = gs ? *(BYTE **)(gs + 0x20) : NULL;
        if (sys) cnt = *(int *)(sys + 0x30);
    } __except (EXCEPTION_EXECUTE_HANDLER) { cnt = -1; }
    return cnt;
}

static int DumpDevices(const char *why)
{
    BYTE *gs = NULL, *sys = NULL;
    void **arr = NULL;
    int cnt = 0, i;
    __try {
        gs = *g_gsPtr;
        if (gs) sys = *(BYTE **)(gs + 0x20);
        if (sys) { arr = *(void ***)(sys + 0x28); cnt = *(int *)(sys + 0x30); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("DEVICES(%s): exception reading gs/sys", why); return -1; }
    if (!gs || !sys || !arr) { Log("DEVICES(%s): gs=%p sys=%p arr=%p (not ready)", why, (void *)gs, (void *)sys, (void *)arr); return -1; }
    if (cnt < 0 || cnt > 64) { Log("DEVICES(%s): implausible count %d", why, cnt); return -1; }
    Log("DEVICES(%s): gs=%p sys=%p arr=%p count=%d focus=%s(0x%x)", why, (void *)gs, (void *)sys, (void *)arr, cnt, IdName(g_lastFocus), (unsigned)g_lastFocus);
    for (i = 0; i < cnt; i++) {
        BYTE *dev = NULL, *vt = NULL;
        int type = 0, ok = 1;
        __try { dev = (BYTE *)arr[i]; vt = *(BYTE **)dev; type = *(int *)(dev + 0x1154); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
        if (!ok) { Log("  dev[%d] = %p (unreadable)", i, (void *)dev); continue; }
        {
            DWORD vrva = InImage(vt) ? (DWORD)(vt - g_exe) : 0;
            Log("  dev[%d] = %p vtable=%p (RVA 0x%lx %s) type=0x%x -> %s", i, (void *)dev, (void *)vt, vrva, VtName(vrva), (unsigned)type, IdName(type));
        }
    }
    return cnt;
}

static void DumpOnce(void)
{
    int i;
    char mod[MAX_PATH] = "(null)";
    Log("---- one-shot dump ----");
    for (i = 0; i < g_nIds; i++)
        Log("type id %-8s @RVA 0x%llx = 0x%x", g_ids[i].name, (unsigned long long)((BYTE *)g_ids[i].addr - g_exe), (unsigned)ReadId(g_ids[i].addr));
    if (g_imgSize > RVA_STEAM_B1) {
        BYTE b0 = 0xEE, b1 = 0xEE;
        SafeCopy(&b0, g_exe + RVA_STEAM_B0, 1);
        SafeCopy(&b1, g_exe + RVA_STEAM_B1, 1);
        Log("steam-input bytes exe+0x%x=%02x exe+0x%x=%02x (analysis expects 01/00 => Steam device update inactive)", RVA_STEAM_B0, b0, RVA_STEAM_B1, b1);
    }
    if (g_pXiPtr) {
        void *tgt = NULL;
        SafeCopy(&tgt, g_pXiPtr, sizeof tgt);
        if (tgt) {
            HMODULE hm = NULL;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)tgt, &hm) && hm)
                GetModuleFileNameA(hm, mod, MAX_PATH);
            else strcpy(mod, "(no module)");
        }
        Log("XInputGetState ptr @RVA 0x%llx -> %p module=%s%s", (unsigned long long)((BYTE *)g_pXiPtr - g_exe), tgt, mod, g_xiHooked ? " [currently our hook; this is pre-hook original]" : "");
        if (g_xiHooked && g_origGetState) {
            HMODULE hm = NULL;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)g_origGetState, &hm) && hm)
                GetModuleFileNameA(hm, mod, MAX_PATH);
            Log("XInputGetState original = %p module=%s", (void *)g_origGetState, mod);
        }
    }
    g_lastDevCount = DumpDevices("install");
    Log("---- end dump ----");
}

/* ---------------------------------------------------------------- installation */
static BYTE *g_prevHit[S_COUNT];
static int   g_scanTry, g_stable, g_rotVia;

static void LogSigResult(const SIG *s, const char *name)
{
    if (s->cnt == 1) Log("sig %-20s OK  fn=%p RVA 0x%llx", name, (void *)s->hit, (unsigned long long)(s->hit - g_exe));
    else Log("sig %-20s FAIL matches=%d%s", name, s->cnt, s->optional ? " (optional)" : "");
}

/* try to resolve all signatures; returns 1 when everything required is unique and stable over two passes */
static int ResolveAll(int final)
{
    int i, ok = 1, allOpt = 1, same = 1;
    BYTE *rot = NULL;
    for (i = 0; i < S_COUNT; i++) ScanSig(&g_sig[i]);
    g_rotVia = 0;
    if (g_sig[S_ROT].cnt == 1) rot = g_sig[S_ROT].hit;
    else if (g_sig[S_ROTS].cnt == 1 && InImage(g_sig[S_ROTS].hit - 0x54) && !memcmp(g_sig[S_ROTS].hit - 0x54, "\x48\x8B\xC4", 3)) rot = g_sig[S_ROTS].hit - 0x54;
    if (!rot) ok = 0;
    for (i = S_MOUSE; i < S_COUNT; i++) {
        if (g_sig[i].cnt != 1) { if (g_sig[i].optional) allOpt = 0; else ok = 0; }
    }
    if (!ok || (!allOpt && !final)) {
        if (g_scanTry % 20 == 0 || final) {
            Log("signature scan pass %d: not ready (code may still be packed)", g_scanTry);
            for (i = 0; i < S_COUNT; i++) LogSigResult(&g_sig[i], g_sig[i].name);
        }
        g_scanTry++; g_stable = 0;
        return 0;
    }
    g_scanTry++;
    g_sig[S_ROT].hit = rot; g_sig[S_ROT].cnt = rot ? 1 : 0;      /* ROTATE slot holds the resolved function start */
    for (i = 0; i < S_COUNT; i++) if (g_prevHit[i] != g_sig[i].hit) same = 0;
    for (i = 0; i < S_COUNT; i++) g_prevHit[i] = g_sig[i].hit;
    if (!same) { g_stable = 0; return 0; }
    return ++g_stable >= 2;
}

static void InstallHooks(void)
{
    BYTE *rot = g_sig[S_ROT].hit, *mouse = g_sig[S_MOUSE].hit, *inp = g_sig[S_INP].hit, *xiu = g_sig[S_XIU].hit;
    BYTE *slotInp, *slotMouse, *slotRot;
    int cInp, cMouse, cRot, i;
    int sizeMatch = g_exeFileSize == EXPECTED_EXE_SIZE;

    Log("---- signature results ----");
    for (i = 0; i < S_COUNT; i++) LogSigResult(&g_sig[i], g_sig[i].name);
    Log("ROTATE_GetRotation resolved via %s -> fn=%p RVA 0x%llx", g_rotVia ? "ROTATE_short-0x54 fallback" : "full sig", (void *)rot, (unsigned long long)(rot - g_exe));

    /* RIP-relative globals */
    g_gsPtr = (BYTE **)Rel(rot, 0x57, 0x5B);
    g_pMouseId = (int *)Rel(rot, 0xCB, 0xCF);
    g_pKbId = (int *)((BYTE *)g_pMouseId - 0x40);
    g_pXiPtr = (void **)Rel(xiu, 0x26, 0x2A);
    if (g_sig[S_XCTOR].cnt == 1) g_pXenonId = (int *)Rel(g_sig[S_XCTOR].hit, 9, 13);
    if (!InImage(g_gsPtr) || !InImage(g_pMouseId) || !InImage(g_pKbId) || !InImage(g_pXiPtr)) {
        Log("FALLBACK: resolved globals outside the image (gs=%p mouseId=%p kbId=%p xiPtr=%p), no hooks", (void *)g_gsPtr, (void *)g_pMouseId, (void *)g_pKbId, (void *)g_pXiPtr);
        return;
    }
    if (g_pXenonId && !InImage(g_pXenonId)) g_pXenonId = NULL;
    Log("gsINPUT ptr global   RVA 0x%llx (analysis 0x%x) value=%p", (unsigned long long)((BYTE *)g_gsPtr - g_exe), RVA_EXP_GS, *(void **)g_gsPtr);
    Log("mouse type id        RVA 0x%llx = 0x%x (gameplay copy)", (unsigned long long)((BYTE *)g_pMouseId - g_exe), (unsigned)ReadId(g_pMouseId));
    Log("keyboard type id     RVA 0x%llx = 0x%x (mouse - 0x40)", (unsigned long long)((BYTE *)g_pKbId - g_exe), (unsigned)ReadId(g_pKbId));
    if (g_pXenonId) Log("xenon type id        RVA 0x%llx = 0x%x", (unsigned long long)((BYTE *)g_pXenonId - g_exe), (unsigned)ReadId(g_pXenonId));
    else Log("xenon type id        unresolved (XINPUT_ctor sig missing) -> ForceGamepadFocus unavailable");
    if (g_sig[S_XCTOR].cnt == 1) Log("XINPUT_ctor vtable    RVA 0x%llx", (unsigned long long)(Rel(g_sig[S_XCTOR].hit, 0x18, 0x1C) - g_exe));
    if (g_sig[S_GSF].cnt == 1) {
        BYTE *g2 = Rel(g_sig[S_GSF].hit, 0x1C, 0x20);
        Log("gsINPUT cross-check via GSINPUT_FrameUpdate: %p %s", (void *)g2, g2 == (BYTE *)g_gsPtr ? "== OK" : "!= MISMATCH");
        if (g2 != (BYTE *)g_gsPtr) { Log("FALLBACK: gsINPUT global mismatch, no hooks"); return; }
    }
    if (g_sig[S_ISGP].cnt == 1) Log("IsGamepadFocus fn      RVA 0x%llx", (unsigned long long)(g_sig[S_ISGP].hit - g_exe));
    Log("XInputGetState ptr   RVA 0x%llx (analysis 0x4f119a8)", (unsigned long long)((BYTE *)g_pXiPtr - g_exe));
    if (sizeMatch && ((BYTE *)g_gsPtr - g_exe != RVA_EXP_GS))
        Log("WARNING: exe size matches the analysed build but gsINPUT RVA differs from analysis");

    g_nIds = 0;
    AddId("mouse", g_pMouseId); AddId("keyboard", g_pKbId); AddId("xenon", g_pXenonId);
    if (g_imgSize > RVA_GENERIC + 4) {
        AddId("mouse", (int *)(g_exe + RVA_MOUSE_ORIG)); AddId("keyboard", (int *)(g_exe + RVA_KB_ORIG));
        AddId("xenon", (int *)(g_exe + RVA_XENON)); AddId("ds4", (int *)(g_exe + RVA_DS4)); AddId("generic", (int *)(g_exe + RVA_GENERIC));
    }

    /* vtable slots */
    cInp = FindSlot(inp, &slotInp); cMouse = FindSlot(mouse, &slotMouse); cRot = FindSlot(rot, &slotRot);
    Log("vtable slot INPSYS_Update: fn=%p matches=%d slot=%p RVA 0x%llx (analysis 0x%x)", (void *)inp, cInp, (void *)slotInp, (unsigned long long)(slotInp ? slotInp - g_exe : 0), RVA_EXP_SLOT_INP);
    Log("vtable slot MOUSE_Update : fn=%p matches=%d slot=%p RVA 0x%llx (analysis 0x%x)", (void *)mouse, cMouse, (void *)slotMouse, (unsigned long long)(slotMouse ? slotMouse - g_exe : 0), RVA_EXP_SLOT_MSE);
    Log("vtable slot ROTATE       : fn=%p matches=%d slot=%p RVA 0x%llx (analysis 0x%x)", (void *)rot, cRot, (void *)slotRot, (unsigned long long)(slotRot ? slotRot - g_exe : 0), RVA_EXP_SLOT_ROT);

    /* install in dependency order: INPSYS (frame ids) -> ROTATE (injection) -> MOUSE (suppression, only safe if injection works) */
    if (cInp == 1) {
        g_origInp = (PFN_Upd)inp;
        if (PatchSlot(slotInp, (void *)HookInpSysUpdate, inp)) { g_hkInp = 1; Log("hook INPSYS_Update INSTALLED slot=%p orig=%p -> %p", (void *)slotInp, (void *)inp, (void *)HookInpSysUpdate); }
        else Log("hook INPSYS_Update: patch failed (slot content changed or VirtualProtect err=%lu)", GetLastError());
    } else Log("hook INPSYS_Update NOT installed (slot matches=%d)", cInp);

    if (g_hkInp && cRot == 1) {
        g_origRot = (PFN_Rot)rot;
        if (PatchSlot(slotRot, (void *)HookGetRotation, rot)) { g_hkRot = 1; Log("hook ROTATE INSTALLED slot=%p orig=%p -> %p", (void *)slotRot, (void *)rot, (void *)HookGetRotation); }
        else Log("hook ROTATE: patch failed (err=%lu)", GetLastError());
    } else Log("hook ROTATE NOT installed (inpsys hooked=%ld, slot matches=%d)", g_hkInp, cRot);

    if (g_hkRot && cMouse == 1) {
        g_origMouse = (PFN_Upd)mouse;
        if (PatchSlot(slotMouse, (void *)HookMouseUpdate, mouse)) { g_hkMouse = 1; Log("hook MOUSE_Update INSTALLED slot=%p orig=%p -> %p", (void *)slotMouse, (void *)mouse, (void *)HookMouseUpdate); }
        else Log("hook MOUSE_Update: patch failed (err=%lu)", GetLastError());
    } else Log("hook MOUSE_Update NOT installed (rotate hooked=%ld, slot matches=%d) -> mouse motion NOT suppressed", g_hkRot, cMouse);

    if (!g_hkInp || !g_hkRot || !g_hkMouse)
        Log("FALLBACK: not all hooks installed (inp=%ld rot=%ld mouse=%ld); missing parts behave vanilla", g_hkInp, g_hkRot, g_hkMouse);
    else Log("all hooks installed");
    g_hooksDone = 1;
}

/* swap the game's XInputGetState function pointer; retried until the game has loaded xinput9_1_0.dll */
static void TryXInput(void)
{
    void *cur = NULL;
    DWORD old;
    static int warned;
    if (!g_pXiPtr) return;
    if (!SafeCopy(&cur, g_pXiPtr, sizeof cur)) return;
    if (!cur) {
        if (!warned) { warned = 1; Log("XInputGetState pointer still NULL (game has not loaded xinput yet), will retry"); }
        return;
    }
    if (cur == (void *)HookGetState) return;
    g_origGetState = (PFN_GetState)cur;
    if (!VirtualProtect(g_pXiPtr, 8, PAGE_READWRITE, &old)) { Log("XInputGetState ptr: VirtualProtect failed err=%lu", GetLastError()); return; }
    InterlockedExchangePointer((PVOID volatile *)g_pXiPtr, (PVOID)HookGetState);
    VirtualProtect(g_pXiPtr, 8, old, &old);
    if (g_xiHooked) Log("XInputGetState pointer was replaced by the game: re-hooked, new orig=%p", cur);
    else Log("XInputGetState pointer hooked: orig=%p -> %p", cur, (void *)HookGetState);
    InterlockedExchange(&g_xiHooked, 1);
}

/* ---------------------------------------------------------------- init / worker */
static void InitAll(void)
{
    char *p;
    HANDLE fh;
    LARGE_INTEGER sz;
    IMAGE_NT_HEADERS *nt;

    strcpy(g_dir, "");
    GetModuleFileNameA(g_self, g_ini, MAX_PATH);
    p = strrchr(g_ini, '\\');
    if (p) {
        char c = p[1];
        p[1] = 0; strcpy(g_dir, g_ini); p[1] = c;
        strcpy(g_logPath, g_dir); strcat(g_logPath, "mixed_input.log");
        strcpy(g_oldLogPath, g_dir); strcat(g_oldLogPath, "mixed_input.old.log");
        strcpy(p + 1, "xinput_mouse.ini");
    }
    LoadSettings();
    if (g_logOn && g_logPath[0]) {
        MoveFileExA(g_logPath, g_oldLogPath, MOVEFILE_REPLACE_EXISTING);
        g_logFile = fopen(g_logPath, "w");
    }

    g_exe = (BYTE *)GetModuleHandleA(NULL);
    nt = (IMAGE_NT_HEADERS *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
    g_imgSize = nt->OptionalHeader.SizeOfImage;
    GetModuleFileNameA(NULL, g_exePath, MAX_PATH);
    fh = CreateFileA(g_exePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (fh != INVALID_HANDLE_VALUE) {
        if (GetFileSizeEx(fh, &sz)) g_exeFileSize = (ULONGLONG)sz.QuadPart;
        CloseHandle(fh);
    }
    Log("SM2 dual-input dinput8 proxy loaded (x64). exe='%s' base=%p imageSize=0x%lx fileSize=%llu (analysed build %llu, %s)",
        g_exePath, (void *)g_exe, g_imgSize, g_exeFileSize, EXPECTED_EXE_SIZE, g_exeFileSize == EXPECTED_EXE_SIZE ? "match" : "DIFFERENT BUILD");
    Log("ini='%s'", g_ini);
    Log("settings: Enabled=%ld GyroSensitivity=%.3f CameraMatch=%d TrackpadOverride=%d StickOverrideDeadzone=%.2f InvertX=%d InvertY=%d SwapAxes=%d PadIndex=%d ForceGamepadFocus=%d Log=%d Diag=%d",
        g_enabled, (double)g_gyroSens, g_cameraMatch, g_trackpadOv, (double)g_override, g_invX, g_invY, g_swap, g_padIndex, g_forceFocus, g_logOn, g_diag);
}

static void EnsureReal(void)
{
    if (g_real) return;
    EnterCriticalSection(&g_initCs);
    if (!g_real) {
        char sys[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);
        strcat(sys, "\\dinput8.dll");
        g_real = LoadLibraryA(sys);
    }
    LeaveCriticalSection(&g_initCs);
}

static void StatusLine(void)
{
    int focus = g_lastFocus, i, n = g_instCount;
    char per[256];
    size_t len = 0;
    per[0] = 0;
    for (i = 0; i < n && len + 40 < sizeof per; i++)
        if (g_inst[i].match) {
            int w = _snprintf(per + len, sizeof per - len, "%s%p:%lld", len ? " " : "", g_inst[i].p, (long long)g_inst[i].inj);
            if (w < 0) break;
            len += (size_t)w;
        }
    Log("STATUS enabled=%ld focus=%s(0x%x) switches=%lld frames=%ld mouseUpd=%lld suppressed=%lld sum(dx,dy)=(%.1f,%.1f) | rot calls=%lld inst=%ld matched=%ld stick=%.3f injected=%lld [%s] injMaxPerFrame=%ld override=%lld exc=%lld gsNull=%lld",
        g_enabled, IdName(focus), (unsigned)focus, s_switches, g_frameId, s_mouseCalls, s_suppressed, s_sumDx, s_sumDy,
        s_rotCalls, g_instCount, s_matched, (double)s_stick, s_injected, per, s_injMaxFrame, s_override, s_exc, s_gsNull);
    if (g_diag)
        Log("STATUS2 out before=(%.5f,%.5f) after=(%.5f,%.5f) k=%.6f [mouseSens=%.4f x 0.05 x scale=%.4f x look=%.4f x gyro=%.3f] invertByte=%ld | swap=%d invX=%d invY=%d cameraMatch=%d",
            (double)s_beforeA, (double)s_beforeB, (double)s_afterA, (double)s_afterB, (double)s_k, (double)s_kSens, (double)s_kScale, (double)s_kLook, (double)g_gyroSens,
            s_invByte, g_swap, g_invX, g_invY, g_cameraMatch);
}

static DWORD WINAPI WorkerThread(LPVOID unused)
{
    int tick;
    double last[8];
    (void)unused;
    EnsureReal();
    InitAll();
    memset(last, 0xFF, sizeof last);
    /* wait (up to ~60 s) until the real code is present (SteamStub may still be decrypting) */
    for (tick = 0; tick < 240 && !g_hooksDone; tick++) {
        if (!g_text && !FindSections()) { Sleep(250); continue; }
        /* sections found once; after 20 s accept missing optional sigs */
        if (ResolveAll(tick >= 80)) {
            InstallHooks();
            break;
        }
        Sleep(250);
    }
    if (!g_hooksDone) Log("FALLBACK: code never matched the signatures in 60 s (different game build or still packed); game behaves vanilla");
    if (g_hooksDone) {
        TryXInput();
        if (g_diag) DumpOnce();
    }
    for (;;) {
        double cur[8];
        Sleep(1000);
        if (!g_hooksDone) continue;
        TryXInput();                                  /* retried until non-NULL, re-hooked if the game replaces it */
        if (g_diag) {
            int n = DevCount();                           /* cheap probe: dump again only when the count changed */
            if (n != g_lastDevCount && n >= 0) g_lastDevCount = DumpDevices("device count changed");
        }
        cur[0] = (double)s_suppressed; cur[1] = (double)s_injected; cur[2] = (double)s_override; cur[3] = (double)s_switches;
        cur[4] = (double)g_enabled + (double)s_matched * 2.0 + (double)(int)(s_stick * 10.0f) * 8.0; cur[5] = s_sumDx; cur[6] = s_sumDy; cur[7] = (double)s_exc;
        if (!memcmp(cur, last, sizeof cur)) continue;
        memcpy(last, cur, sizeof cur);
        StatusLine();
    }
    return 0;
}

/* ---------------------------------------------------------------- dinput8 exports (forwarded) */
typedef HRESULT (WINAPI *PFN_Create)(HINSTANCE, DWORD, const GUID *, LPVOID *, void *);
typedef HRESULT (WINAPI *PFN_Void)(void);
typedef HRESULT (WINAPI *PFN_GetClass)(const GUID *, const GUID *, LPVOID *);
typedef const void *(WINAPI *PFN_GetDf)(void);

HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, const GUID *riid, LPVOID *out, void *outer)
{
    PFN_Create f;
    EnsureReal();
    f = g_real ? (PFN_Create)GetProcAddress(g_real, "DirectInput8Create") : NULL;
    return f ? f(inst, ver, riid, out, outer) : E_FAIL;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    PFN_Void f;
    EnsureReal();
    f = g_real ? (PFN_Void)GetProcAddress(g_real, "DllCanUnloadNow") : NULL;
    return f ? f() : S_FALSE;
}

HRESULT WINAPI DllGetClassObject(const GUID *c, const GUID *i, LPVOID *p)
{
    PFN_GetClass f;
    EnsureReal();
    f = g_real ? (PFN_GetClass)GetProcAddress(g_real, "DllGetClassObject") : NULL;
    return f ? f(c, i, p) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllRegisterServer(void)
{
    PFN_Void f;
    EnsureReal();
    f = g_real ? (PFN_Void)GetProcAddress(g_real, "DllRegisterServer") : NULL;
    return f ? f() : E_FAIL;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    PFN_Void f;
    EnsureReal();
    f = g_real ? (PFN_Void)GetProcAddress(g_real, "DllUnregisterServer") : NULL;
    return f ? f() : E_FAIL;
}

const void *WINAPI GetdfDIJoystick(void)
{
    PFN_GetDf f;
    EnsureReal();
    f = g_real ? (PFN_GetDf)GetProcAddress(g_real, "GetdfDIJoystick") : NULL;
    return f ? f() : NULL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        int i;
        DisableThreadLibraryCalls(inst);
        g_self = inst;
        QueryPerformanceFrequency(&g_freq);
        InitializeCriticalSection(&g_logCs);
        InitializeCriticalSection(&g_initCs);
        for (i = 0; i < S_COUNT; i++) ParseSig(&g_sig[i]);
        CloseHandle(CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL));
    }
    return TRUE;
}
