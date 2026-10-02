# mouse-hardware-fix

A lightweight, plug-and-play fix for a worn-out mouse, for **Linux and Windows**:

* **Scroll wheel bounce**: the wheel sometimes jumps the wrong way while you scroll.
* **Button chatter**: one click registers as a double click, or a drag drops partway through.

It mainly targets the Logitech G Pro series, but works with any mouse:

<img src="https://raw.githubusercontent.com/Aleynikovich/mouse-hardware-fix/main/resources/reddit.png" alt="Reddit Logo" width="400"/>

## How it works

**Clicks.** Presses go through immediately, so clicking has no extra latency. Each release is held back for a short window (25 ms by default). If the switch bounces back to "pressed" during that window, the bounce and the release are both discarded. This removes ghost double clicks when you click and when you let go, and drags and held buttons don't get interrupted by chatter partway through. Real double clicks are far slower than 25 ms and are unaffected.

**Scrolling.** While you are scrolling, a single step in the opposite direction is treated as encoder bounce. By default it is *inverted* rather than dropped, so scrolling keeps a steady pace without a hitch. If the wheel keeps turning the other way (2 notches by default), it counts as a real reversal. Any pause longer than 300 ms ends the gesture, so reversing after a short pause is always instant. High-resolution wheels are measured in fractions of a notch, so they behave the same as normal wheels.

Anything the filters don't touch (movement, horizontal scroll, extra buttons, and so on) is passed through unchanged.

## Linux

The daemon grabs the mouse's evdev node and replaces it with a `uinput` clone that copies the original's name, vendor/product IDs and capabilities. libinput, hwdb DPI entries, desktop per-device settings and tools like Piper keep working. A udev rule starts one instance per physical mouse. Touchpads, pointing sticks, tablets, touchscreens, joysticks, virtual devices and anything with LEDs or force feedback are skipped. The systemd unit is sandboxed and can only reach its own input node and `/dev/uinput`. If the daemon stops for any reason, the kernel releases the grab and the mouse goes straight back to normal.

### Arch Linux (AUR)
```bash
paru -S mouse-hardware-fix-git
```
or manually:
```bash
git clone https://aur.archlinux.org/mouse-hardware-fix-git.git
cd mouse-hardware-fix-git
makepkg -si
```

### Other distributions
Prerequisites: a C++17 compiler, systemd, udev.

```bash
git clone https://github.com/Aleynikovich/mouse-hardware-fix.git
cd mouse-hardware-fix
g++ -std=c++17 -O2 mouse-hardware-fix.cpp -o mouse-hardware-fix

sudo install -Dm755 mouse-hardware-fix /usr/bin/mouse-hardware-fix
sudo install -Dm644 mouse-hardware-fix@.service /etc/systemd/system/mouse-hardware-fix@.service
sudo install -Dm644 99-mouse-fix.rules /etc/udev/rules.d/99-mouse-fix.rules
sudo install -Dm644 mouse-hardware-fix.conf /etc/mouse-hardware-fix.conf

sudo systemctl daemon-reload
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=input --action=add
```

CMake works too: `cmake -B build && cmake --build build && sudo cmake --install build`.

### Configuration
Edit `/etc/mouse-hardware-fix.conf` and put [flags](#options) in `EXTRA_ARGS`. To fix only one mouse and leave every other device alone, use `--only <vid:pid>` (IDs from `lsusb`):

```bash
EXTRA_ARGS="--only 046d:c08b"
```

Apply changes with `sudo systemctl restart 'mouse-hardware-fix@*'`. Logs go to `journalctl -u 'mouse-hardware-fix@*'`.

### Uninstall
```bash
sudo rm /usr/bin/mouse-hardware-fix /etc/systemd/system/mouse-hardware-fix@.service \
        /etc/udev/rules.d/99-mouse-fix.rules /etc/mouse-hardware-fix.conf
sudo systemctl stop 'mouse-hardware-fix@*'
sudo systemctl daemon-reload && sudo udevadm control --reload-rules
```

## Windows

On Windows it runs as a small background program per user, using a low-level mouse hook. There's no driver and no admin rights. Corrected events are re-injected with `SendInput`. Input injected by other software (remote desktop, AutoHotkey and so on) is never touched.

### Build
With Visual Studio (Developer PowerShell) or MinGW:
```powershell
cmake -B build
cmake --build build --config Release
```
or directly with MSVC:
```powershell
cl /O2 /EHsc /std:c++17 mouse-hardware-fix-windows.cpp /Fe:mouse-hardware-fix.exe /link /SUBSYSTEM:WINDOWS user32.lib advapi32.lib shell32.lib
```

### Use
```powershell
.\mouse-hardware-fix.exe --verbose          # try it out, logs corrections to the console
.\mouse-hardware-fix.exe --install          # start at every login (with any options you add) and start now
.\mouse-hardware-fix.exe --stop             # stop the running instance
.\mouse-hardware-fix.exe --uninstall        # remove from login and stop
```
`--install` stores the command line in `HKCU\...\CurrentVersion\Run`. To change options, run `--install` again with the new flags. Keep the `.exe` in a permanent location before you install it.

Limitations of a user-mode hook:
* The hook sees all mice combined. It can't target a single device the way `--only` does on Linux.
* Windows doesn't let a non-elevated program filter input aimed at elevated (administrator) windows. Start it from an elevated prompt if you need that.
* Games that read raw input directly may still see the original, unfiltered events.

## Options

| Flag | Default | Description |
| --- | --- | --- |
| `--disable-scroll` | | Turn off the scroll wheel fix. |
| `--disable-click` | | Turn off the click chatter fix. |
| `--scroll-timeout <sec>` | `0.300` | Pause that ends a scroll gesture. After it, any direction is accepted immediately. |
| `--scroll-reversal <n>` | `2` | Notches the wheel must keep turning the other way mid-gesture before it counts as a real reversal. |
| `--scroll-drop` | | Drop bounced wheel steps instead of inverting them. |
| `--click-timeout <sec>` | `0.025` | Chatter window. Raise it if double clicks still slip through, lower it if you jitter-click. |
| `--verbose` | | Log every corrected event. |
| `--only <vid:pid>` | | **Linux only.** Handle only this device (repeatable). |
| `--device <path>` | | **Linux only.** The evdev node, normally supplied by the udev rule. |

## Tests

The filter logic is platform-independent ([mouse-filter.hpp](mouse-filter.hpp)) and has unit tests:
```bash
g++ -std=c++17 -I. tests/filter-test.cpp -o filter-test && ./filter-test
```
