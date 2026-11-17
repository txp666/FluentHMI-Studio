# Tutorial projects / 视频教程工程

These `.uidp` files open in the desktop HMI designers. The older numbered examples keep their original protocol setup; the version snapshot below applies only to the new S7 lesson.

这些 `.uidp` 工程可由桌面设计器打开。旧教程保留原有协议配置；下列版本快照只针对本期 S7 教程，不代表已重新验证全部旧工程。

## 13 — Your first S7 PLC / 第一次 S7 PLC 联调

[Watch video lesson12](https://hmi.modusignal.cn/tutorials/12-s7-plc/?lang=en) · [观看第12期视频](https://hmi.modusignal.cn/tutorials/12-s7-plc/) · [English instructions](../../docs/tutorials/13-s7-plc.en.md) · [中文步骤](../../docs/tutorials/13-s7-plc.zh.md)

| File / 文件 | Open with / 打开方式 |
| --- | --- |
| [13-S7-PLC-Monitor-en.uidp](13-S7-PLC-Monitor-en.uidp) | English HMI project / 英文 HMI 工程 |
| [13-S7PLC联调.uidp](13-S7PLC联调.uidp) | Chinese HMI project / 中文 HMI 工程 |
| [13-s7-first-plc.plcsim.json](13-s7-first-plc.plcsim.json) | [FluentPLC Simulator](https://github.com/txp666/FluentPLC-Simulator) project / 模拟器工程 |

**Version snapshot / 版本快照 (2026-11-16):** FluentHMI Studio C++ **0.7.0**, PyQt UI Designer **0.7.0**, FluentPLC Simulator **0.1.1**, UIDP **1.1**. Website lesson slug: `12-s7-plc`; source example number: `13`.

1. Open the `.plcsim.json` in Simulator while stopped; start its loopback server on `127.0.0.1:1102`.
2. Open either `.uidp` in desktop HMI 0.7.0. Run Preview and manually connect: rack 0, slot 1, PG. Auto-connect is off.
3. Read temperature **23.5 °C**, speed **900 rpm**, and a pressure sine wave **0.8–1.6 bar**.
4. Toggle Pump enable. Enter **1500** and press Enter, then press the **900 rpm** preset. Confirm the display reads the new value back from the simulator.
5. Save and export: C++ creates an independent runtime application; Py exports editable Python source or builds with Nuitka.

操作：先在模拟器载入并启动工程，再在 HMI 打开中文或英文工程，运行预览并手动连接。观察数值和曲线，拨动开关，输入 1500 后回车，再用按钮恢复 900。两份语言工程的控件 ID、地址和命令相同；模拟器的 `.plcsim.json` 与 HMI 的 `.uidp` 是不同格式。

| Address / 地址 | Type / 类型 | Purpose / 用途 |
| --- | --- | --- |
| `DB1.DBX0.0` | BOOL | Pump enable feedback and command / 泵启停读写 |
| `DB1.DBX0.7` | BOOL | Neighbor bit initially ON / 邻位初值为 ON |
| `DB1.DBW2` | INT | Speed, initially 900 rpm / 转速初值 900 |
| `DB1.DBD4` | REAL | Temperature, 23.5 °C / 温度 |
| `DB1.DBD8` | REAL | Dynamic pressure / 动态压力与曲线 |

The tutorial validates a local simulator, not physical Siemens hardware. It uses classic S7comm absolute addresses, not optimized DBs, symbolic access or S7CommPlus. [Connection setup and scope](../../docs/s7.md).

## Updating this lesson / 后续维护

Update the two language projects, simulator address/initial values, address table, software versions and validation date together. Keep historical version records for older lessons unless they have actually been revalidated. Regenerate screenshots and video from the recorded software versions when changing visible behavior.

本期网站素材存放于 `tutorials/assets/12-s7-plc/v070/`。视频源工程本地保留在 `video-workflow/projects/FluentHMI-Studio/tutorials/12-s7-plc/`，不放入软件源码仓库。教程工程在软件 v0.7.0 发布后新增，下载链接应使用教程提交 / 主分支或网站静态资源，不能假设它们存在于软件 `v0.7.0` tag 内。

This directory contains examples and documentation only. Changes to it use `[skip ci]`; validate edited project files locally rather than rerunning desktop packaging CI for prose changes.

## v0.8 示例 / v0.8 examples

[中文操作图解](../../docs/tutorials/v0.8-guide.zh.md) · [English illustrated guide](../../docs/tutorials/v0.8-guide.en.md)

### 14 · 工程数据与报警

打开 [14-工程数据与报警.uidp](14-工程数据与报警.uidp) 并预览，启动两路 TCP 服务端（9201、9202）。用客户端接入后，设备 A 每行发送 `fault=0,soc=65`，设备 B 每行发送 `fault=0`。将任一 `fault` 改为 1 可触发总故障，`soc` 改为 20 可触发低电量。点击设计器顶部「数据与报警」可查看数据点、表达式与报警规则。

Start the two TCP servers in Preview and connect with local clients. The example combines both devices' fault states, shows battery charge, and records alarm events. See the illustrated guide for the sample messages and configuration.

### 15 · 分页与控件风格

打开 [15-分页与控件风格.uidp](15-分页与控件风格.uidp) 并预览，无需硬件或连接。四列分别演示经典选项卡、下划线导航、分段导航和文档页签；点击「总览 / 参数」切换静态读数。中间比较标准、强调、轻量和描边按钮，点击仅显示本地通知；下方比较标准、透明和描边卡片及数值卡。

返回设计器后，选中分页容器，在属性区切换导航风格，添加、删除、重命名或前后移动页面。用「当前编辑页」选设计器正在编辑的页，用「运行时启动页」选预览和导出应用的首页。页内控件随页面移动，删除页面时移至相邻页。按钮和卡片也可在属性区直接选择风格。

Open this project and run Preview without a device or connection. Compare four page navigation styles, four button styles and three card styles. Buttons show local notifications only. Select a container in the designer to change its style or manage its pages.

### 16 · 输入与状态筛选

打开 [16-输入与状态筛选.uidp](16-输入与状态筛选.uidp) 并预览，无需硬件。上方对比小数数值框（含六位精度）、下拉／分段／单选组、开关／复选框；这些演示输入不发送设备命令。中间的状态徽标复用枚举文字与颜色。

下方使用固定计算值模拟温度报警，压力报警保留无数据状态。点击「筛选」组合关键词、级别、状态和时间。切换到「历史记录」后可按事件类型筛选，也可选「自定义时间」分别填写起止日期和时间。「导出筛选结果」保存匹配的事件，「导出全部历史」保存完整历史。重新运行预览后，固定温度值会再次满足报警条件。

Open this project without a device to compare decimal inputs, selector styles, checkboxes and status badges. A constant calculated temperature demonstrates alarms; pressure remains unknown. Expand Filter to combine conditions, or switch to history to filter events. Demo inputs do not send device commands.
