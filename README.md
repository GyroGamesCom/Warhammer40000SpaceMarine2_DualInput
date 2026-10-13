# Dual Input for Warhammer 40,000: Space Marine 2

Gyro aiming and gamepad at the same time. No stuttering as before. Gyro moves the camera like a real mouse
(linear, no stick curve), and the game keeps showing gamepad buttons.

> **Offline only. Do not use it in multiplayer.**
> The game has Easy Anti-Cheat (EAC). The mod works only with EAC off, so use it for
> the campaign and offline PvE only. Never try to play online or PvP with the mod;
> mods in an anti-cheat protected game can get your account banned.
> Uninstall the mod and clear the launch option before playing online.

## What it does

- Gyro goes straight into the camera as native mouse look.
- The gamepad stays the active device (gamepad prompts, no flickering).
- The right stick/trackpad has priority: while you push it, gyro is ignored.
- Menus are not affected.
- **F10** turns the mod on/off in game.

## Install

1. Close the game. Run `install.bat`.
2. Set the Steam launch option (this starts the game without Anti-Cheat):
   Steam > right-click the game > **Properties** > **General** > **Launch Options**,
   paste this line (`install.bat` prints it with your path and copies it to the clipboard):

   ```
   "<game folder>\client_pc\root\bin\pc\Warhammer 40000 Space Marine 2 - Retail.exe" %command%
   ```

   Example:
   `"D:\SteamLibrary\steamapps\common\Space Marine 2\client_pc\root\bin\pc\Warhammer 40000 Space Marine 2 - Retail.exe" %command%`

   Keep `%command%` at the end. Start the game with **Play** as usual.
3. Steam Input (controller layout for the game):
   - Gyro: **As Mouse**
   - Right trackpad / right stick: **Joystick**

**Manual install:** copy `dinput8.dll` and `xinput_mouse.ini` from `mod` to
`Space Marine 2\client_pc\root\bin\pc\`, then do steps 2 and 3.

## Uninstall / back to online

Run `uninstall.bat`, then clear the Launch Options line in Steam.
The game then starts with Anti-Cheat again.

## Settings

`xinput_mouse.ini` next to the DLL. Restart the game after a change.

| Key | Default | |
|---|---|---|
| `GyroSensitivity` | 1.0 | Gyro speed (on top of the in-game look sensitivity) |
| `InvertX` / `InvertY` | 0 | 1 = flip that gyro axis |
| `StickOverrideDeadzone` | 0.27 | How far the right stick must move to override gyro |
| `Enabled` | 1 | 0 = mod off |

## Problems

- **No effect:** check the launch option. After starting the game, `mixed_input.log`
  must appear next to the DLL. If not, the game ran with Anti-Cheat.
- **Gyro does nothing:** touch a stick or pad button once so the game switches to gamepad.
- **Game updated, mod stopped working:** the mod turns itself off when the game code
  changes. Wait for an update.
- Tested on the Steam version, build 25799387.

Source code is in `src` (build with `src\build.bat`, MSVC x64).

Unofficial fan mod, not affiliated with Saber Interactive, Focus Entertainment,
Games Workshop or Valve. Use at your own risk. License: `LICENSE.txt`.
