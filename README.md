# SnowDesk ❄

**English** | [Русский](README.ru.md)

SnowDesk puts snow on your desktop, on Windows and Linux. Flakes fall across the screen and pile up on the top edges of your windows.
When you close, minimize or move a window, the snow on it falls to the ground above the taskbar.
You can scoop up the snow, make snowballs, throw them and stack them into a snowman.

## Getting started

1. **[⬇ Download SnowDesk.exe](https://github.com/nyellswag/SnowDesk/raw/main/download/SnowDesk.exe)**: a single file of about 330 KB that runs on Windows 10 and 11. Put it in any folder and double-click it.
   If the program is on drive C: and you also have a D: drive, it will offer once to move itself to `D:\SnowDesk`.
2. Windows may show "Windows protected your PC". This happens because the app has no paid code-signing certificate.
   Click **More info → Run anyway**.
3. A snowflake icon appears in the system tray, next to the clock. If you don't see it, it may be hidden under the ^ arrow.

## Controls

Snow is interactive all the time. You don't need a special mode, and your normal work isn't affected.

| Action | How |
|---|---|
| Make a snowball | press and hold the left mouse button on a drift (on the ground or on a window) and move the mouse around |
| Throw it | release the button mid-swing; the snowball breaks when it hits a window or the ground |
| Place it | release the button gently; three balls stacked on each other become a snowman |
| Push snow | falling flakes flow around the cursor; a quick swipe ploughs through drifts and kicks snowballs |
| Click through snow | a short click on a drift without moving the mouse goes to the window underneath, as usual |
| Play Mode | **Ctrl+Alt+S**, or left-click the tray snowflake: the mouse plays only with snow, even in empty space. A small "Play Mode" label appears at the top. Leave with Esc, right-click or Ctrl+Alt+S |
| Menu | right-click the tray snowflake |

Windows are solid: a snowball thrown at a window breaks against the side it hits and the snow bounces off and falls; a gently dropped ball can sit on a window's top edge, and you can build a snowman there too. The snowman rides along when you drag the window gently, and falls off if you yank the window hard, minimize it or close it. Thrown snow doesn't stay as a sharp pile: it rolls down and spreads into a low mound. The cursor only shoves snow when you flick it quickly, so slowly moving onto a drift or a snowball to grab it leaves it alone.

The menu has Play Mode, pause, snowfall strength (light, normal, heavy, blizzard),
"Add a snowdrift", "Clear all snow", **"Make snowballs without Play Mode"** and **"Cursor pushes snow"**
(both on by default, either can be turned off), start with Windows, open the program folder,
**Language** (English or Russian), **"Uninstall SnowDesk completely"** and exit.
The interface is in English by default; switch it to Russian in the tray menu under **Language / Язык**.

The snow hides itself while a fullscreen game or video is in front.

## Linux

1. **[⬇ Download snowdesk-linux-x86_64.tar.gz](https://github.com/nyellswag/SnowDesk/raw/main/download/snowdesk-linux-x86_64.tar.gz)** (about 300 KB).
2. Unpack it and start the program:
   ```sh
   tar xzf snowdesk-linux-x86_64.tar.gz
   ./snowdesk/snowdesk
   ```
3. A menu opens once to say hello. Later you open the menu with **Ctrl+Alt+M**, through the tray icon
   (on desktops that have a system tray: KDE, Xfce, Cinnamon, MATE), or by running `snowdesk` again.
   **Ctrl+Alt+S** turns Play Mode on or off.

The snow, snowballs, windows and menu work the same as on Windows.

- **X11 desktops get everything:** KDE Plasma (X11), Xfce, Cinnamon, MATE, and GNOME on Xorg.
- **A compositor is needed** for transparency. GNOME, KDE and Cinnamon have one by default; on Xfce turn it on in
  Settings → Window Manager Tweaks → Compositor.
- **Wayland** (the default on newer Ubuntu and Fedora): SnowDesk runs through XWayland, but Wayland doesn't let programs
  see other apps' windows. The snow falls and piles up at the bottom of the screen, but not on windows.
  For the full effect, choose "GNOME on Xorg" or "Plasma (X11)" on the login screen.
- **Requirements:** a 64-bit PC with glibc 2.27 or newer (Ubuntu 18.04+, Debian 10+, Fedora 28+ and similar),
  plus the usual X11 libraries that every desktop already has.
- **Files:** settings go to `~/.config/snowdesk/`, the saved snow to `~/.local/share/snowdesk/`, and autostart (if you turn it on)
  to `~/.config/autostart/snowdesk.desktop`. "Uninstall SnowDesk completely" in the menu deletes all of these and the program file.
- **Command line:** `snowdesk --menu`, `snowdesk --play`, `snowdesk --quit`.

## Where the data is stored (Windows)

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
- `src/main.cpp`: the Windows version: overlay window, tray icon, menu, autostart, move to D: and self-uninstall
- `src/linux/main_linux.cpp`: the Linux (X11) version: overlay, input shaping, tray icon, menu, shortcuts, autostart, self-uninstall
- `build.sh`: builds the Windows version with mingw-w64, from Linux or from MSYS2 on Windows
- `build_linux.sh`: builds the Linux version (needs `libx11-dev libxext-dev libxrandr-dev libxft-dev libxtst-dev`)
- `tests/test_sim.cpp`: a test that runs the simulation without Windows and saves frames as image files

```sh
./build.sh
```
