# Install, export, and build v0.9

[简体中文](build.zh.md) · [User guide](guide.en.md) · [Architecture](architecture.md)

## Install

Find the package for your operating system and CPU in [GitHub Releases](https://github.com/txp666/FluentHMI-Studio/releases). The package version is independent of the UIDP format version.

| Platform | Package | How to start |
| --- | --- | --- |
| Windows x64 | ZIP | Extract the whole folder and launch `FluentHMIStudio.exe` alongside the runtime executable and Qt libraries |
| macOS Apple Silicon or Intel | Universal ZIP | Extract and move `FluentHMI Studio.app` to Applications |
| Linux x64 or arm64 | DEB | Install with apt and use the launcher or `fluenthmi-studio` |

macOS packages use ad hoc signing. See the release notes for the published package and platform details.

The package includes `docs` and `examples`. Start with `examples/tutorials/README.md` for the offline guide and labs. On macOS, both folders are in the app bundle under `Contents/Resources`. On Linux, look under `/opt/FluentHMIStudio`.

## Exported apps

In Studio, save the project, verify it in Preview, then use Export. The export bundles the runner, project, images, DBC files, theme, accent, and language. Run or distribute the complete export for the target platform and architecture.

- Windows exports a folder with the executable and its dependencies.
- macOS exports an app bundle. Universal Studio includes a universal runtime.
- Linux exports a `.tar.gz` archive. Extract it and use `start.sh`.

Headless export uses Studio's saved appearance settings. Use `QT_QPA_PLATFORM=offscreen` in a session without a display.

```bash
FluentHMIStudio --export out monitor.uidp
FluentHMIRuntime monitor.uidp --check
```

The exported runtime scales the whole canvas to fit the window, with centered margins when the aspect ratio differs. Re-export an existing app to bring its runtime up to v0.9.

## Linux deployment

### Linux package and launcher

Install the `.deb` for x64 or arm64. The package targets glibc 2.39 or later, including Ubuntu 24.04, Debian 13, and Raspberry Pi OS trixie. Check the distribution and architecture on embedded boards before installing.

```bash
sudo apt install ./FluentHMI-Studio-vX.Y.Z-linux-x64.deb
```

apt installs Studio and its system libraries under `/opt/FluentHMIStudio`. Start it from the application menu or with `fluenthmi-studio`. The launcher defaults to X11, including XWayland under Wayland. Set `QT_QPA_PLATFORM` to override it.

### Linux archives and kiosk deployment

A Linux export contains an executable, `start.sh`, the packaged Qt libraries and plugins, `project.uidp`, `appearance.json`, and any assets. Extract the archive before opening the launcher. The following example uses a project named `MyHMI`.

```bash
tar -xzf MyHMI.tar.gz
./MyHMI/start.sh
```

`start.sh` runs the app from its own folder, on X11 when there is a display and `QT_QPA_PLATFORM` is not set. To use the app on another machine of the same architecture, copy the archive there (`scp`, a USB stick) and unpack it. Install any missing system libraries on the target. The runtime requires glibc 2.39 or later.

```bash
sudo apt install libgl1 libopengl0 libegl1 libfontconfig1 libfreetype6 libglib2.0-0t64 libdbus-1-3 \
  libudev1 libssl3t64 libgssapi-krb5-2 libxkbcommon0 libx11-6 libx11-xcb1 libxext6 libxfixes3 libxi6 \
  libxrender1 libsm6 libice6 libxcb1 libxcb-glx0 libxcb-randr0 libxcb-render0 libxcb-shape0 \
  libxcb-shm0 libxcb-sync1 libxcb-xfixes0 libxcb-xinerama0 libxcb-xinput0 libxcb-xkb1 \
  libwayland-client0 libwayland-cursor0 libwayland-egl1
```

Install `fonts-noto-cjk` if the target lacks fonts for Chinese text.

To add the app to the application menu, save the desktop entry below as `~/.local/share/applications/MyHMI.desktop`. This example assumes you extracted the archive in `/home/pi`. Put a copy in `~/.config/autostart/` to start the app on login.

```ini
[Desktop Entry]
Type=Application
Name=MyHMI
Exec=/home/pi/MyHMI/start.sh
Icon=/home/pi/MyHMI/MyHMI.png
StartupWMClass=MyHMI
Terminal=false
```

For a boot-time kiosk without a desktop session, save a systemd unit such as the one below as `/etc/systemd/system/my-hmi.service`. EGLFS uses the GPU's DRM/KMS driver and the system libraries `libdrm2`, `libgbm1`, and `libinput10` for input. Select `linuxfb` for a framebuffer deployment.

```ini
[Unit]
Description=My HMI

[Service]
User=pi
SupplementaryGroups=dialout video render input
Environment=QT_QPA_PLATFORM=eglfs
ExecStart=/home/pi/MyHMI/start.sh
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable --now my-hmi.service
```

Export for Linux using Studio on a Linux machine with the target architecture. From an SSH session without a display, run the export with the offscreen platform.

```bash
QT_QPA_PLATFORM=offscreen fluenthmi-studio --export out project.uidp
```

### Serial ports

Check the group on the serial device node, such as `/dev/ttyUSB0` or `/dev/ttyACM0`. On Debian, Ubuntu, and Fedora it is commonly `dialout`. Arch commonly uses `uucp`. With a dialout device, add the current account and log in again.

```bash
sudo usermod -aG dialout "$USER"
```

If the device disappears or stays busy after insertion, check kernel logs and whether a service such as BRLTTY or ModemManager has opened it.

## CAN adapters

Install the driver or library for your adapter. Virtual CAN is built into Studio on all three platforms.

| Adapter | Windows | macOS | Linux |
| --- | --- | --- | --- |
| PEAK (PCAN-USB, PCAN-USB FD) | PEAK's device driver, which installs `PCANBasic.dll` | the PCBUSB library from [mac-can.com](https://mac-can.github.io/drivers/libPCBUSB.html) (`libPCBUSB.dylib` in `/usr/local/lib`) | choose SocketCAN: the kernel's `peak_usb` driver |
| ZLG (USBCAN-I/II, USBCAN-E-mini, USBCAN-E-U family, USBCANFD) | ZLG's device driver, and from the `zlgcan_x64` folder of ZLG's "CAN(FD) interface function library" `zlgcan.dll` with its `kerneldlls` folder, next to the program or in a `zlgcan` folder beside it. The library needs the Visual C++ 2013 x64 runtime (USBCAN-I/II also the 2008 x64 runtime with MFC). zlgcan.dll looks for each device's kernel DLL on the system path first, then in `kerneldlls`; the library has none for the USBCAN-E-U family (`USBCAN_E_64.dll` and the like). The card names any missing file when it connects | – | – |
| SocketCAN | – | – | built in; the interface's bitrate is set with `ip link` (the card shows the command, e.g. `sudo ip link set can0 up type can bitrate 500000 sample-point 0.875`) |
| slcan (CANable, CANUSB and other Lawicel-protocol adapters) | the adapter's serial port | the adapter's serial port | the adapter's serial port; classic CAN only, 10k–1M |
| Virtual CAN | Built-in loopback | Built-in loopback | Built-in loopback |

[Configure timing, DBC signals, and recording](uidp/can.md)

## Build from source

Use CMake 3.21 or later, a C++17 compiler, and Qt 6.12 with Core, Gui, Widgets, Svg, Network, SerialPort, and Test when running tests. Studio finds Qt under the default installer paths. Pass `-DCMAKE_PREFIX_PATH=/path/to/qt` for a different location. On Linux install `libgl1-mesa-dev`, `libxcb-cursor0` and `libxkbcommon-x11-0`.

```bash
git clone --recursive https://github.com/txp666/FluentHMI-Studio.git
cd FluentHMI-Studio
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

In VS Code, open the repository to build with Ctrl/Cmd+Shift+B or start the debugger with F5. On Windows, open VS Code from Developer PowerShell and choose the MSVC Studio configuration. Run `open build/debug/FluentHMIStudio.app` on macOS or `build/debug/FluentHMIStudio` on Linux.

Regenerate the widget reference from the source directory after editing widget metadata. The docs test checks that both generated sections are current.

```bash
build/debug/uidp_reference docs
FluentHMIRuntime docs/images/widgets-en.uidp --theme dark --screenshot widgets-dark.png
```

## Online updates

Studio checks GitHub Releases from Settings. The default startup check displays a prompt when a newer version is available. You can update now, skip the version, or defer it.

The updater verifies the package size and SHA-256 from the release. Windows and macOS prepare an update beside the installation, then replace it after Studio exits. The helper restores the previous installation if replacement fails. For builds and protected installations, Studio opens the download page.

On Linux the updater verifies the `.deb` and uses pkexec and apt-get for an installation under `/opt/FluentHMIStudio`. The updated Studio starts as the original user. You can also run `FluentHMIStudio --update`. Developers can point `FHMI_UPDATE_URL` at a local JSON fixture in the GitHub latest-release format.

## Packaging and releases

```bash
cmake --preset release
cmake --build --preset release
python3 scripts/package.py --build-dir build/release --output-dir dist
```

The packager shares Qt between Studio and the standalone runner and checks that an exported demo starts. The packaged macOS runner is at `Contents/Helpers/FluentHMIRuntime.app`. Windows and Linux place it beside Studio. Linux packaging uses patchelf and checks dynamic dependencies from the finished DEB and exported archive. Run the packager on a desktop session or under `xvfb-run -a` on Linux.

`CMakeLists.txt` is the application version source. The build writes `FluentHMIStudio-version.txt` for packaging. Match the release notes file and release tag to that version, such as `docs/release-notes/v0.9.0.md` and `v0.9.0`. The release workflow validates Windows, universal macOS, and x64/arm64 Linux packages. A tagged run publishes only after all packaging checks pass. Manual and pull request runs produce checks and artifacts.
