# Space Marine 2 (Swarm engine) — input system analysis for the dual-input mod

Static analysis (Ghidra via REA + Python RIP-xref/AOB scans). Tags: [obs] read from code, [inf] inferred, [live] verify in memory.

## Target
- `client_pc\root\bin\pc\Warhammer 40000 Space Marine 2 - Retail.exe`
- SHA-256 `3dc44c2ff0b7d313ebcc78d28ff0e0b665f468983fafa7ab880b03eaca65b3bf`, image base 0x140000000, ASLR on → always module base + RVA.
- Entry point in `.bind` (SteamStub). `.text` plaintext in file; all signatures verified against file bytes.
- `Retail.dll` = server build (`DLL_CreateServer`), not used by the client.
- 0-byte `Retail.exe.local` (DotLocal) exists in `bin\pc`.

## 1. Mouse — DirectInput8 only, absolute axis mode [obs]
No raw input anywhere.
- InputManager init `0x141d54590`: `DirectInput8Create(...,0x800)` → `this+0x50`. EnumDevices callback `0x141d567d0`; type 0x12 → `inpDEVICE_MOUSE` (vtable `0x14364e1b8`, size 0x1608).
- Mouse Init (slot 1, `0x141d55560`): GUID_SysMouse, c_dfDIMouse2, DIPROP_BUFFERSIZE=30, DIPROP_AXISMODE=ABS.
- Mouse Update (slot 4, `0x141d557c0`): `GetDeviceState(0x14, dev+0x15ec)`; `dx = lX - g_prevX(0x144f11fe0)`, `dy = lY - g_prevY(0x144f11fe4)` (globals shared by all mice). float dx `+0x15dc`, dy `+0x15e0`, wheel*0.1 `+0x15e4`. Channels: `+0x90` X+, `+0x94` X-, `+0x98` Y+, `+0x9c` Y-, `+0xa0/+0xa4` wheel. Buttons `+0x10..+0x2c`. GetDeviceData only latches quick clicks.

Generic `inpDEVICE`: `+0x10` float channel[0x154]; `+0x560` flags (0x08 = ignore for activity); `+0xc04` previous channels; `+0x1154` type id; `+0x1178` IDirectInputDevice8*.
Vtable: +0x20 Update, +0x80 SetFocus, +0x88 IsGamepad (`0x140244870`→1, `0x1402467a0`→0), +0x90 HasActivity.

`inpSYSTEM` (vtable `0x14364e000`): `+0x28` device array, `+0x30` count, `+0x38` SRW lock, `+0x40` last-activity tick. Update = slot 4 `0x141d547e0` (copy channels→prev, dev->Update, HasActivity).

Type-id globals: mouse `0x144f11760`, keyboard `0x144f1172c`, gamepad_xenon `0x144f11728`, gamepad_ds4 `0x144f11764`, gamepad_generic `0x144f118f8`. Gameplay copies: mouse `0x144dea380`, keyboard `0x144dea340`.

## 2. Gamepad
- XInput [obs]: `LoadLibraryA("xinput9_1_0.dll")` (bare name, `0x141d54600`). Pointers: SetState `0x144f119a0`, **GetState `0x144f119a8`**, GetCaps `0x144f119b0`.
- XInput devices created from HID enumeration (`0x141d56a90`) for non-Sony usage-5 devices: vtable `0x14364db40`, type gamepad_xenon. Steam virtual pad should land here [inf].
- XInput Update `0x141d51fc0`: index `+0x1240`, connected `+0x123c`, XINPUT_GAMEPAD at `+0x1228`, RX/RY at `+0x1230/+0x1232`.
- DS4/DS5 via HID.DLL (vtables `0x14364d728`, `0x14364d970`).
- ISteamInput (SteamInput006) present but Steam device update `0x141d52cc0` requires bytes `0x144f11698` and `0x144f11699`; no static write to `...699` → probably inactive [inf, medium].

## 3. Active (focus) device [obs]
gsINPUT singleton `*(int**)0x144dea4f0`: `+0x00` int focusDeviceTypeId; `+0x04` changed flag; `+0x08` switch timestamp; `+0x20` inpSYSTEM*; `+0x28/+0x30` command list/count; `+0xe0` byte invert mouse look; `+0xe4` float LookSensitivity; `+0xe8/+0xf0` focus listeners.
- Per-frame `0x1412cd8f0`: inpSYSTEM->Update → `0x1412ce270` per command → `0x1412cbad0` notify listeners (UI/glyphs) + `dev->SetFocus(IsGamepadFocus())`.
- Switch: command value above threshold → focus = device type of the binding that fired. Only KB↔mouse hysteresis (`Input.MouseFocusSwitchDelay`, value `0x144dea3a8`). Any mouse-axis movement → mouse focus immediately; any pad command → back.
- `IsGamepadFocus()` `0x1412cc4c0`; callers: rotate, `0x1412d1840`, `0x1412d19d0`, `0x1412ea190`, `0x1412ea550`, `0x1412ea880`, `0x1412cbb8f`.
- `InputActionRotate` chooses settings by focus type (`0x1412d1620`); each flip swaps settings, resets spline + acceleration.

## 4. Camera look — `InputActionRotate::GetRotation` [obs]
- Vtable `0x1435e8300`, slot 5 (slot `0x1435e8328`) = **`0x1412d0b60`** (only reference).
- `void(this, float dt /*xmm1*/, float* outA /*r8*/, float* outB /*r9*/)`.
- `A = cmd[this+0x48].v - cmd[this+0x40].v`, `B = cmd[this+0x50].v - cmd[this+0x58].v` (value at cmd+8; `this+0x7c` enabled byte). `mag = clamp(len(A,B),0,1)`.
- Mouse focus: linear, no dt; moving-average filters at `this+0x98`/`this+0x80`; `k = (*(this+0x38))->f[0x130] * 0.05 * this->f[0x78]`; A negated if invert.
- Gamepad focus: radial deadzone (settings+0x88), ellipse (+0x8c/+0x90), spline (`this+0xe8`), acceleration (`0x1412d10e0`), × speed (settings+0x78/+0x7c) × dt.
- Output: `*outA = gs->sens(+0xe4)*A` (pitch? [inf]), `*outB = gs->sens*B` (yaw? [inf]).

**Stutter cause [inf, high]:** with gamepad focus the mouse counts go through the stick model, are clamped to full deflection, × speed × dt → on/off motion depending on whether a gyro packet arrived that frame. Plus focus flips mouse↔pad, each resetting settings/acceleration and swapping glyphs.

## 5. Design
Loader: `dinput8.dll` proxy in `bin\pc` (static import).
- A. Patch inpSYSTEM slot 4 (`0x14364e020`) to bracket the frame; patch mouse vtable slot 4 (`0x14364e1d8`): after orig, if enabled && gamepad focus, accumulate `+0x15dc/+0x15e0`, zero them and channels `+0x90..+0x9f` (keep wheel/buttons). Focus stays on pad; stick model never sees mouse.
- B. Patch rotate slot 5 (`0x1435e8328`): after orig, add `k*dy`/`k*dx` to outA/outB with native mouse scale `k = (*(this+0x38))[+0x130]*0.05*this[+0x78]*gs[+0xe4]*userMult`; skip if right stick above override threshold. Signs/axes [live].
- C. Optional forced gamepad focus: `*(int*)gs = *(int*)0x144f11728; gs[+4]=1` (breaks native mouse look → opt-in).

## 6. AOB signatures (unique in .text; RIP operand = match + next + int32@(match+disp))
- ROTATE_GetRotation → `0x1412d0b60`:
  `48 8B C4 53 56 57 41 54 41 57 48 81 EC D0 00 00 00 0F 29 70 C8 4D 8B F9 0F 29 78 B8 4D 8B E0 44 0F 29 40 A8 48 8B F9 44 0F 29 48 98 44 0F 29 50 88 44 0F 28 D1 44 0F 29 74 24 40 44 0F 29 7C 24 30 48 89 68 10 4C 89 70 18 44 0F 29 64 24 60 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 45 0F 57 C9 41 0F 94 C6 80 7F 7C 00`
  gs ptr disp +0x57 next +0x5B → `0x144dea4f0`; IsGamepadFocus disp +0x5C next +0x60 → `0x1412cc4c0`; mouse type id disp +0xCB next +0xCF → `0x144dea380`.
- ROTATE_short (fn+0x54): `48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 45 0F 57 C9 41 0F 94 C6 80 7F 7C 00`
- MOUSE_Update → `0x141d557c0` (slot `0x14364e1d8`):
  `48 8B C4 53 48 81 EC 20 03 00 00 48 8B D9 48 8B 89 78 11 00 00 48 85 C9 0F 84 ?? ?? ?? ?? 48 89 78 10 4C 8D 83 EC 15 00 00 0F 29 70 E8 BA 14 00 00 00 0F 29 78 D8 48 8B 01 FF 50 48`
- INPSYS_Update → `0x141d547e0` (slot `0x14364e020`):
  `48 89 5C 24 18 48 89 6C 24 20 56 48 83 EC 20 80 79 20 00 48 8B D9 74 05 E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 74 37 48 8D 0D ?? ?? ?? ?? FF 15 ?? ?? ?? ?? 48 83 38 00 74 24`
- XINPUT_Update → `0x141d51fc0` (GetState ptr disp +0x26 next +0x2A → `0x144f119a8`):
  `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B D9 33 D2 48 83 C1 10 41 B8 50 05 00 00 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 8B BB 3C 12 00 00`
- XINPUT_ctor → `0x141d51e40` (xenon id disp +9 next +13 → `0x144f11728`; vtable disp +0x18 next +0x1C → `0x14364db40`):
  `40 53 48 83 EC 20 48 8D 15 ?? ?? ?? ?? 48 8B D9 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? C7 83 40 12 00 00 00 00 00 00 48 89 03`
- GSINPUT_FrameUpdate → `0x1412cd8f0` (gs ptr disp +0x1C next +0x20):
  `48 8B C4 55 53 41 54 48 8B EC 48 83 EC 60 48 89 70 08 45 33 E4 48 89 78 10 48 8B 3D ?? ?? ?? ?? 4C 89 70 E0`
- CMD_UpdateFocus → `0x1412ce270`: `40 53 48 83 EC 70 0F B6 51 10 48 8B D9 8B 49 10 80 E2 01 8B C1 0F 29 7C 24 20 C1 E8 04`
- ISGAMEPADFOCUS → `0x1412cc4c0`: `40 53 48 83 EC 40 48 8B D9 C6 44 24 50 00 48 8D 44 24 50 48 C7 44 24 28 00 00 00 00 0F 57 C0 48 89 44 24 58 48 8D 4C 24 58 F3 0F 7F 44 24 30 E8`

Vtable slots: search `.rdata` (RVA 0x3508000, size 0xABC75A) for the absolute runtime function address (each unique).

## 7. Other data
MouseFocusSwitchDelay obj `0x144dea388` value `0x144dea3a8`; LookSensitivity `0x144dea420`; IsInvertMouseLook `0x144dea470`; GamepadRightStickDeadZone obj `0x144dea3c8`; Debug.MouseInputScaleNeeded `0x144f116f0` (scale X `0x144f11920`, Y `0x144f11818`); WndProc handler `0x1412cb360`.

## 8. Live checks
1. `gs=*(void**)(exe+0x4dea4f0)`; watch `*(int*)gs` vs type ids.
2. Walk devices (`sys=gs+0x20`, array +0x28, count +0x30): vtables, +0x1154 types; mouse +0x15dc/+0x15e0; XInput +0x123c/+0x1240/+0x1228; Steam pad = XInput class?
3. Bytes exe+0x4f11698/699 (expect 1/0); target of exe+0x4f119a8.
4. GetRotation: calls/frame, instances, dt, yaw/pitch mapping + signs, this+0x78, (*(this+0x38))+0x130, gs+0xe4, gs+0xe0.
5. Gamepad focus + mouse only: cmd values at this+0x40..0x58 non-zero?
6. Runtime code bytes == file after SteamStub.
7. MouseFocusSwitchDelay value. 8. Menus/cursor dependence on DI channels.

## 9. Unknowns
Writer of `0x144f11699`; `0x1412ea*` consumers; binding tables; outA/outB yaw/pitch assignment.

## 10. Live findings (build 85129256, 2026-10-11)
- Current RVAs: GetRotation 0x12e8100, rotate vtable 0x3606090 (slot5 +0x28), Rotate::Update (slot1) 0x12e8000, gs ptr 0x4e0b540, mouse id 0x4e0b3d0 (=0x35c), kb 0x350, xenon 0x372, ds4 0x364, xbox_one 0x4bd56. All AOBs still unique.
- 15 InputActionRotate objects; in menus 6 active (MoveCursor x2, MoveCursorGamepadOnly x2, MainMenuWeaponRotate, MainMenuWeaponZoom). Injecting into all broke menu navigation.
- Gameplay look = keymap `layers/Game/sections/Camera/commands/Rotation`; inactive in menu (this+0x28 = 0).
- Bindings: cmd = *(this+0x40|48|50|58) (fwd, back, left, right); n=*(int*)(cmd+0x28), entries=*(cmd+0x20) stride 0x28; entry+0x08 -> int devIds[*(int*)(entry+0x10)], entry+0x18 -> int keys[*(int*)(entry+0x20)].
  Game camera: fwd = mouse 35 / pad 38, back = 34 / 39, left = 33 / 37, right = 32 / 36; desc(+0x38)+0x130 sens 1.2. MainMenuWeaponRotate has pad 38/39 swapped.
- Steam Input: with Steam Input on, the pad is the Steam device class (vtable RVA 0x366bc18, ISteamInput), NOT XInput -> XInputGetState hook sees nothing. Read the stick from command values instead.
