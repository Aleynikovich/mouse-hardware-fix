# mouse-hardware-fix

A lightweight, plug-and-play C++ daemon for Linux that fixes physical scroll wheel bouncing and mouse button chatter (double-clicking) at the kernel level. This is specifically targeting the pesky Logitech G Pro series:

<img src="https://raw.githubusercontent.com/Aleynikovich/mouse-hardware-fix/main/resources/reddit.png" alt="Reddit Logo" width="400"/>
## Configuration

After installation, you can customize the daemon's behavior by editing the global configuration file:

`/etc/mouse-hardware-fix.conf`

You can pass extra arguments to the daemon by modifying the `EXTRA_ARGS` variable. Supported flags:
* `--disable-scroll`: Disables the scroll wheel fix.
* `--disable-click`: Disables the mouse button debounce fix.
* `--scroll-timeout <sec>`: Sets the idle window for scroll direction resets (default: 0.400).
* `--click-timeout <sec>`: Sets the debounce window for click signals (default: 0.025).

## Installation

### Arch Linux (AUR)
Using an AUR helper:
```bash
paru -S mouse-hardware-fix-git
```

Manual AUR installation:
```bash
git clone [https://aur.archlinux.org/mouse-hardware-fix-git.git](https://aur.archlinux.org/mouse-hardware-fix-git.git)
cd mouse-hardware-fix-git
makepkg -si
```

### Manual Installation (Debian/Ubuntu/Fedora/etc.)
Prerequisites: g++, systemd, udev

1. Clone and compile:
```bash
git clone [https://github.com/Aleynikovich/mouse-hardware-fix.git](https://github.com/Aleynikovich/mouse-hardware-fix.git)
cd mouse-hardware-fix
g++ -O3 mouse-hardware-fix.cpp -o mouse-hardware-fix
```

2. Install components:
```bash
sudo cp mouse-hardware-fix /usr/bin/
sudo cp mouse-hardware-fix@.service /etc/systemd/system/
sudo cp 99-mouse-fix.rules /etc/udev/rules.d/
sudo cp mouse-hardware-fix.conf /etc/mouse-hardware-fix.conf
```

3. Reload and trigger:
```bash
sudo systemctl daemon-reload
sudo udevadm control --reload-rules
sudo udevadm trigger
```

## How It Works



The daemon intercepts raw hardware signals using `EVIOCGRAB`, filters bounces based on your configured timeouts, and outputs clean signals via `uinput`. To prevent infinite udev loops, the virtual device is tagged with a vendor ID (`1234`) which the `99-mouse-fix.rules` is hardcoded to ignore.
