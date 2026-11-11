# SnowDesk ❄

**English** | [Русский](README.ru.md)

SnowDesk puts snow on your Windows desktop. Flakes fall across the screen and pile up on the top edges of your windows.
When you close, minimize or move a window, the snow on it falls to the ground above the taskbar.
You can scoop up the snow, make snowballs, throw them and stack them into a snowman.

## Getting started

1. **[⬇ Download SnowDesk.exe](https://github.com/nyellswag/SnowDesk/raw/main/download/SnowDesk.exe)**: a single file of about 330 KB that runs on Windows 10 and 11. Put it in any folder and double-click it.
   If the program is on drive C: and you also have a D: drive, it will offer once to move itself to `D:\SnowDesk`.
2. Windows may show "Windows protected your PC". This happens because the app has no paid code-signing certificate.
   Click **More info → Run anyway**.
3. A snowflake icon appears in the system tray, next to the clock. If you don't see it, it may be hidden under the ^ arrow.

## Controls

| Action | How |
|---|---|
| Turn play mode on or off | **Ctrl+Alt+S**, or left-click the tray snowflake |
| Make a snowball | in play mode, hold the left mouse button on the snow and move the mouse around |
| Throw it | release the button mid-swing; the snowball breaks when it hits a window or the ground |
| Place it | release the button gently; three balls stacked on each other become a snowman |
| Leave play mode | right-click, Esc or Ctrl+Alt+S; play mode also turns itself off after 60 seconds without mouse movement |
| Menu | right-click the tray snowflake |

The menu has pause, snowfall strength (light, normal, heavy, blizzard), "Add a snowdrift", "Clear all snow",
start with Windows, open the program folder, **"Uninstall SnowDesk completely"** and exit.
The menu is in Russian for now.

Outside play mode, clicks pass straight through the snow, so it never gets in the way of your work.
The snow hides itself while a fullscreen game or video is in front.

## Where the data is stored

Everything is kept next to `SnowDesk.exe`: `SnowDesk.ini` holds the settings and `snow.dat` holds the saved snow and snowmen.
There is no installer and no shortcut, and the app does not touch system files.
The only registry entry is created when you turn on start-with-Windows yourself
(`HKCU\...\CurrentVersion\Run\SnowDesk`).

## Uninstalling

Open the menu, choose "Uninstall SnowDesk completely" and click "Yes". The app closes, removes its autostart entry
and deletes its own files and folder. Nothing else is touched: the folder is removed only if it is named `SnowDesk`
and nothing else is left in it.

## Resource usage (estimated, 1920×1080 screen)

- Memory: about 15–30 MB while running (about 45 MB on a 4K screen), and 0 after you exit.
- CPU: about 1–3% at 30 frames per second. Play mode runs at 60 frames per second and uses a little more.
  When paused or behind a fullscreen app, usage is close to 0.
- Size: one file of about 330 KB with no dependencies.

## Building from source

The code is C++17 on the plain Win32 API, with no third-party libraries.

- `src/sim.cpp`, `src/sim.h`: the snow simulation, which doesn't depend on Windows
- `src/main.cpp`: the overlay window, tray icon, menu, autostart, move to D: and self-uninstall
- `build.sh`: builds with mingw-w64, from Linux or from MSYS2 on Windows
- `tests/test_sim.cpp`: a test that runs the simulation without Windows and saves frames as image files

```sh
./build.sh
```
