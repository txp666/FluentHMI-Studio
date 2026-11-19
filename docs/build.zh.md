# v0.9 安装、部署与构建

[English](build.en.md) · [使用手册](guide.zh.md) · [架构](architecture.md)

## 安装

从 [GitHub Releases](https://github.com/txp666/FluentHMI-Studio/releases) 选择对应系统与 CPU 的包。软件安装包版本和 UIDP 文件格式版本分别管理。

| 系统 | 安装包 | 启动 |
| --- | --- | --- |
| Windows x64 | ZIP | 完整解压，在 Qt 库及运行器所在目录运行 `FluentHMIStudio.exe` |
| macOS Apple Silicon 或 Intel | 通用 ZIP | 解压后将 `FluentHMI Studio.app` 移到「应用程序」 |
| Linux x64 或 arm64 | DEB | 用 apt 安装，从应用菜单或运行 `fluenthmi-studio` |

macOS 包采用 ad hoc 签名。各版本的实际安装包与平台信息以发布页为准。

安装包随附 `docs` 和 `examples`。从 `examples/tutorials/README.md` 可打开本地使用手册与分步实训。macOS 上这两个目录位于应用包的 `Contents/Resources`，Linux 上位于 `/opt/FluentHMIStudio`。

## 导出独立应用

保存工程，先在运行预览确认收发和显示，再用 Studio 的导出功能生成独立应用。导出包带运行器、工程、图片、DBC、主题、强调色和语言设置。部署时复制完整产物，在对应系统与架构运行。

- Windows 生成含程序和依赖的目录。
- macOS 生成 `.app`。通用 Studio 包含通用运行器。
- Linux 生成 `.tar.gz`。解压后运行 `start.sh`。

命令行导出使用 Studio 已保存的外观设置。SSH 下没有显示环境时可设置 `QT_QPA_PLATFORM=offscreen`。

```bash
FluentHMIStudio --export out monitor.uidp
FluentHMIRuntime monitor.uidp --check
```

独立应用按比例缩放整个画布，宽高比不同时居中留边。已有独立应用可重新导出以随包更新到 v0.9 运行器。

## Linux 部署

### Linux 安装与启动

选用对应 x64 或 arm64 的 `.deb`。包要求 glibc 2.39 或更高版本，适用于 Ubuntu 24.04、Debian 13 和 Raspberry Pi OS trixie 等。开发板系统请先核对发行版和架构。

```bash
sudo apt install ./FluentHMI-Studio-vX.Y.Z-linux-x64.deb
```

apt 安装 Studio 和系统依赖，Studio 位于 `/opt/FluentHMIStudio`。从应用菜单或运行 `fluenthmi-studio` 启动。启动器默认使用 X11，在 Wayland 桌面上通过 XWayland 运行。可以用 `QT_QPA_PLATFORM` 选择其他平台。

### Linux 压缩包与开机运行

Linux 归档包含程序、`start.sh`、Qt 库与插件、`project.uidp`、`appearance.json` 和工程资源。解压后运行启动脚本。下面以项目名 `MyHMI` 为例。

```bash
tar -xzf MyHMI.tar.gz
./MyHMI/start.sh
```

`start.sh` 在应用自己的文件夹里运行它；有显示环境且没有设置 `QT_QPA_PLATFORM` 时用 X11。要在同架构的另一台机器上用，把压缩包拷过去（`scp`、U 盘）再解压。目标机器需要 glibc 2.39 或以上版本。按需安装缺少的系统库。

```bash
sudo apt install libgl1 libopengl0 libegl1 libfontconfig1 libfreetype6 libglib2.0-0t64 libdbus-1-3 \
  libudev1 libssl3t64 libgssapi-krb5-2 libxkbcommon0 libx11-6 libx11-xcb1 libxext6 libxfixes3 libxi6 \
  libxrender1 libsm6 libice6 libxcb1 libxcb-glx0 libxcb-randr0 libxcb-render0 libxcb-shape0 \
  libxcb-shm0 libxcb-sync1 libxcb-xfixes0 libxcb-xinerama0 libxcb-xinput0 libxcb-xkb1 \
  libwayland-client0 libwayland-cursor0 libwayland-egl1
```

需要中文字体时，安装 `fonts-noto-cjk`。

下面假设解压目录是 `/home/pi/MyHMI`。将桌面项保存为 `~/.local/share/applications/MyHMI.desktop`，即可从菜单打开应用。复制到 `~/.config/autostart/` 可以登录时启动。

```ini
[Desktop Entry]
Type=Application
Name=MyHMI
Exec=/home/pi/MyHMI/start.sh
Icon=/home/pi/MyHMI/MyHMI.png
StartupWMClass=MyHMI
Terminal=false
```

无桌面开机全屏运行时，把下面的 systemd 服务保存为 `/etc/systemd/system/my-hmi.service`。EGLFS 使用 GPU 的 DRM/KMS 驱动，需要 `libdrm2`、`libgbm1`，触摸或按键使用 `libinput10`。帧缓冲部署可改用 `linuxfb`。

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

在相同架构的 Linux 上运行 Studio 导出。通过 SSH 且没有显示环境时，可使用离屏模式。

```bash
QT_QPA_PLATFORM=offscreen fluenthmi-studio --export out project.uidp
```

### 串口权限

先检查串口设备节点所属用户组，如 `/dev/ttyUSB0` 或 `/dev/ttyACM0`。Debian、Ubuntu、Fedora 通常使用 `dialout`，Arch 常用 `uucp`。使用 dialout 时，可以按下面操作把当前账号加入该组，再注销并登录。

```bash
sudo usermod -aG dialout "$USER"
```

插入设备后端口消失或一直被占用时，检查内核日志，以及 BRLTTY 或 ModemManager 等服务是否打开了设备。

## CAN 适配器

使用实体卡前安装对应驱动或库。虚拟 CAN 已内置，可用来运行 17.7 实训。

| 适配器 | Windows | macOS | Linux |
| --- | --- | --- | --- |
| PEAK（PCAN-USB、PCAN-USB FD） | PEAK 的设备驱动（会装好 `PCANBasic.dll`） | [mac-can.com](https://mac-can.github.io/drivers/libPCBUSB.html) 的 PCBUSB 库（`libPCBUSB.dylib` 装在 `/usr/local/lib`） | 选 SocketCAN：内核自带 `peak_usb` 驱动 |
| ZLG（USBCAN-I/II、USBCAN-E-mini、USBCAN-E-U 系列、USBCANFD） | 致远的设备驱动，以及致远“CAN(FD) 接口卡二次开发接口函数库” `zlgcan_x64` 文件夹里的 `zlgcan.dll` 和 `kerneldlls` 文件夹，放在程序旁边或旁边的 `zlgcan` 文件夹里。函数库需要 Visual C++ 2013 x64 运行库（USBCAN-I/II 还要 2008 x64 含 MFC 的运行库）。zlgcan.dll 先在系统路径里找各设备的内核 DLL，再找 `kerneldlls`；USBCAN-E-U 系列的（`USBCAN_E_64.dll` 等）函数库里没有。连接时卡片会指出缺哪个文件 | – | – |
| SocketCAN | – | – | 系统自带；接口的波特率用 `ip link` 设置（卡片上会给出命令，如 `sudo ip link set can0 up type can bitrate 500000 sample-point 0.875`） |
| slcan（CANable、CANUSB 等 Lawicel 协议的适配器） | 适配器的串口 | 适配器的串口 | 适配器的串口；只支持经典 CAN，10k–1M |
| 虚拟 CAN | 内置回环 | 内置回环 | 内置回环 |

[配置位时序、DBC 信号和自动保存](uidp/can.md)

## 从源码构建

需要 CMake 3.21 或更高版本、C++17 编译器和 Qt 6.12 的 Core、Gui、Widgets、Svg、Network、SerialPort。运行测试还需要 Qt Test。CMake 自动查找默认的 Qt 安装目录。其他路径加 `-DCMAKE_PREFIX_PATH=/path/to/qt`。Linux 另装 `libgl1-mesa-dev`、`libxcb-cursor0` 和 `libxkbcommon-x11-0`。

```bash
git clone --recursive https://github.com/txp666/FluentHMI-Studio.git
cd FluentHMI-Studio
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

VS Code 打开源码目录后，Ctrl/Cmd+Shift+B 构建，F5 启动调试。Windows 从 Visual Studio 的 Developer PowerShell 启动 VS Code，再选择 MSVC 调试配置。macOS 启动可用 `open build/debug/FluentHMIStudio.app`，Linux 可直接运行 `build/debug/FluentHMIStudio`。

修改控件属性后，从源码目录重新生成控件参考。`docs` 测试会校验两段生成内容。

```bash
build/debug/uidp_reference docs
FluentHMIRuntime docs/images/widgets.uidp --theme light --screenshot widgets-light.png
```

## 在线更新

Studio 从 GitHub Releases 获取更新，可在设置中手动检查。默认的启动检查会在发现新版本后提示更新、跳过或稍后处理。

下载会核对发布页的文件大小与 SHA-256。Windows 和 macOS 在安装目录旁准备更新，退出后替换。如果替换失败，辅助程序会恢复上一版。构建目录或受保护的安装目录使用发布页下载。

Linux 验证 `.deb` 后，通过 pkexec 和 apt-get 更新 `/opt/FluentHMIStudio` 中的安装，重启时以原用户运行。可以使用 `FluentHMIStudio --update`。开发时可用 `FHMI_UPDATE_URL` 指向本机 GitHub latest-release 格式的 JSON 响应来测试更新流程。

## 打包与发布

```bash
cmake --preset release
cmake --build --preset release
python3 scripts/package.py --build-dir build/release --output-dir dist
```

Studio 与运行器共用 Qt。脚本会用打包后的 Studio 导出示例并检查导出的应用能否启动。macOS 的运行器在 `Contents/Helpers/FluentHMIRuntime.app`，Windows 和 Linux 的运行器在 Studio 同目录。Linux 打包用 patchelf 配置库路径，还会检查最终 DEB 和导出包的实际动态库依赖。可在桌面会话中运行，或在 Linux 使用 `xvfb-run -a`。

软件版本由 `CMakeLists.txt` 提供。构建生成 `FluentHMIStudio-version.txt` 供打包读取。发布说明和标签使用相同版本，如 `docs/release-notes/v0.9.0.md` 对应 `v0.9.0`。工作流会校验 Windows 包、macOS 通用包和 Linux x64/arm64 DEB。标签运行会在打包检查全部通过后生成 GitHub Release。手动与 PR 运行生成检查结果和构建产物。
