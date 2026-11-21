# FluentHMI Studio v0.9 user guide

[简体中文](guide.zh.md) · [Project home](https://github.com/txp666/FluentHMI-Studio) · [Install and deploy](build.en.md) · [v0.9.0 release notes](release-notes/v0.9.0.md#english)

Build a device interface on the canvas, set a receive source and a send destination for each control, then exercise the project in Preview. Export packages the same project and runtime for use as a standalone desktop application.

## First run

1. Install Studio for your operating system and architecture. Use **Settings → Language** and restart to change the interface language. [Installation and platform requirements](build.en.md#install)
2. Open [17 Data I/O and frame templates](../examples/tutorials/17-数据收发与帧模板.uidp) and start Preview. Connect the simulated device on the right first, then the HMI client on the left. Both use `127.0.0.1:39172`.
3. Enter `1500` in the target speed input. Click the send-speed button, check the destination and value in the dialog, and confirm. The device's received target updates.
4. Click the feedback button on the right and confirm. The HMI shows `1500 rpm` and adds a chart sample. Change the target to `2300`, press Enter, and send feedback again. The record table shows both target values.
5. Open **Project data monitor** to check each point's source, quality, and timestamp. Use the receive cards to inspect the actual bytes. [Full frame examples and troubleshooting](tutorials/17-frame-io.en.md#17-share-one-frame-template)

## Build a round trip from a blank project

Use two TCP cards in one project to send a line such as `rpm=1500\n`. The hardware can remain disconnected while you practice.

| Canvas item | Settings |
| --- | --- |
| TCP card `hmi` | Client, address `127.0.0.1`, port `39190` |
| TCP card `device` | Server, same address and port |
| Line edit | Under **Actions and sending**, choose Text and target `hmi`. Enter `rpm={value}` and select `\n` as the newline. Disable **Input only** |
| Data receive card | Select `device` as the source connection |
| Numeric display | Under **Receiving and binding**, select `device` and key-value matching, enter the key `rpm`, and set the displayed unit to `rpm` |

Keep the project text parser and let both connections follow the project. Start Preview, connect the server first, and connect the client. Type `1500` and press Enter. The log contains `rpm=1500` and the numeric display reads `1500 rpm`. If port 39190 is in use, set both connection cards to the same free port before connecting.

Save the received speed as a project data point to use it in both the display and a curve. To return simulated feedback, give a button the destination `device` and use the point as the value in an outgoing `rpm` message. Bind a separate display to `hmi` to read that reply. [Data points and parameters](uidp/alarms.md#数据点)

## Connections and routing

Create one connection card per device connection. Use the card ID when setting a source or destination. Give a control separate receive and send settings when it both displays feedback and issues commands.

| Transport | Start with | Practice |
| --- | --- | --- |
| Serial | Port, baud, data bits, parity, stop bits, and flow control. Virtual serial A/B run inside one project | [17.11 Virtual serial](tutorials/17-frame-io.en.md#1711-virtual-serial) |
| TCP | Server listen address and port or a client's destination. Select a peer on servers with multiple clients | [17.10 Mixed receive templates](tutorials/17-frame-io.en.md#1710-mixed-receive-templates) |
| UDP | Pair each local port with the other end's destination port. Match the source by address and port | [17.2 Nested JSON](tutorials/17-frame-io.en.md#172-udp-and-nested-json) |
| MQTT | Broker, port, subscription, and publish topic. Match a received topic with the source endpoint | [17.12 MQTT topics](tutorials/17-frame-io.en.md#1712-mqtt-topics) |
| CAN / CAN FD | Adapter, device and channel, nominal bit rate, and FD data bit rate. Load a DBC for named signals | [CAN configuration](uidp/can.md) |
| Classic S7 | PLC IPv4 address, port, rack, slot, and connection type. The binding then selects an absolute address and type | [13 S7 with a simulator](tutorials/13-s7-plc.en.md) |

When exactly one compatible send destination exists, Studio can select it automatically. Choose a connection and endpoint explicitly when there are several candidates. Use Broadcast for deliberate fan-out. Logs can aggregate traffic. A device data point used by expressions, alarms, or sending needs a unique source. [Routing and endpoints](uidp/data.md#连接与子通道)

## Shared frame templates

1. In **Data and alarms → Frame templates**, create a HEX or text/JSON template. For HEX, set the framing and checksum, then define the command discriminator and fields. Text templates specify encoding, separators, and record boundaries.
2. Paste a complete example in the visual editor. Select bytes or a field row to find its counterpart. Switch to **Bits** to inspect each byte from `B7` through `B0` and highlight the selected field. In the byte strip, the dashed blue line marks the bytes counted by the length field and the solid orange line marks the checksum input. Field offsets start after the header. The preview byte indices, length offset, and checksum start use the whole frame.
3. Select the saved template under **Receive frame template** on the connection card. To receive several HEX layouts on the same stream, select several compatible templates. Give them distinguishable headers or command bytes. Each matched template supplies its own frame boundaries and checksum.
4. In a display, curve, or project data point, select the source connection and bind a template field. With the three templates in 17.10, use `speed.value`, `temperature.value`, and `status.value` to distinguish fields with the same name.
5. Select the same template in a control's send settings. A button can assign inputs, project points, and constants to several fields. A line edit can send one specified field on Enter. Set a shared default such as `cmd` on the template. Mark values such as `rpm` required, then use Send preview to verify the assembled message.

For the frame `AA 55 03 10 DC 05 C5`, `03` counts `10 DC 05`, including command byte `10`. The little-endian `uint16` in `DC 05` is `1500`. In lesson 17.9 the CRC8 covers `03 10 DC 05` and equals `C5`. Confirm the impact list before changing a shared field. A confirmed rename updates references when the editor can identify the field unambiguously. [Frame configuration reference](uidp/connections.md#hex-二进制帧)

## Commands, confirmation, and periodic tasks

| Task | Where to set it |
| --- | --- |
| Send one input on Enter | In the line edit's send settings, choose the mode and destination. Disable **Input only** |
| Send several inputs with a button | Open **Edit send configuration**. Create inputs or select existing ones, then assemble text, JSON, HEX, or raw CAN data |
| Confirm a write | Enable confirmation. Enable its details option to review the destination, values, and command. Both options default to off |
| Write several consecutive Modbus registers | Set the button to FC16. Check the starting address and the register ranges shown for each input. S7 exposes a single-value write form |
| Repeat a send | Set an interval in milliseconds on a supported button or data send card. The runtime control starts and stops the task |

Use Send preview before connecting to equipment to check target and encoding. Periodic sends read current inputs each cycle. A line edit linked to a project parameter commits on Enter or focus loss. Invalid input skips the cycle and appears in the task list. The task continues once the input becomes valid. **Periodic tasks** in Preview or in the exported app lets you inspect and stop running tasks across pages. Disconnecting the target or leaving runtime stops the jobs. [Commands and periodic sending](uidp/commands.md)

## Project data and alarms

Open **Data and alarms** to define device readings, operator parameters, and calculated points once. For example, name two points `AFault` and `BFault` and set a calculated point to `T["AFault"] or T["BFault"]`. Use a source connection and endpoint for each device point. Set a validity period where late data should go stale. The monitor reports the point's last value, quality, source, and time.

1. Add an alarm rule and choose a point and threshold. The threshold editor can generate a condition for you.
2. Add an alarm banner or list to the canvas. In Preview, acknowledge or mute an event and open its history. An acknowledgement and a return to normal are recorded as separate events.
3. Expand the history filter to combine severity, event type, and time. The custom range supports a calendar, separate hour/minute/second input, and direct typing. Choose filtered CSV or all history.

In [14 Project data and alarms](../examples/tutorials/14-工程数据与报警.uidp), connect clients to the local TCP servers at `9201` and `9202`. Send `fault=0,soc=65` to A and `fault=0` to B, with one newline per record. Set either `fault` to `1` to trigger the combined alarm or send `soc=20` to A to trigger a low battery alarm. [Project data and alarm reference](uidp/alarms.md)

## Pages and widget appearance

Page containers offer classic tabs, underlined navigation, segmented navigation, and document tabs. **Current edit page** sets the design canvas page. **Runtime start page** is the first page shown in Preview and the standalone application. Add, rename, and reorder pages from the container's properties.

Numeric displays can use an enum badge with mappings such as `0=Stopped` and `1=Running|#107c10`. Traction battery colors can follow low and warning thresholds. Use [15 Pages and styles](../examples/tutorials/15-分页与控件风格.uidp) and [16 Inputs and status filters](../examples/tutorials/16-输入与状态筛选.uidp) to try selectors, switches, badges, and filtering without hardware.

Set a digital timing chart to **Per-channel binding**, then select a CAN signal or project data point for each of up to 32 channels. In Preview, scroll down the lanes to view more channels. Use the time-window menu to set a custom duration, the folder button to import a recording, or the save menu to export or start logging. Select **Back to live** to return to live data after importing. The Signals and diagnostics page of the dynamic gallery provides a waveform to explore. [Dynamic widget gallery](../examples/industrial/动态控件全景.uidp) · [Timing chart settings and file formats](uidp/electronics.md#共享位定义与复合反馈)

## Inspect traffic and troubleshoot

| What you see | Where to look |
| --- | --- |
| Connected, with no display updates | Check the receive card, then verify the source connection, peer or topic, template field, and byte order |
| Wire bytes with no delivered frames | Enable **Wire bytes** to see framing progress or rejection. Select a row and inspect its sample to check length and checksum |
| Values from different devices appear in one display | Select an explicit source and endpoint. Qualify fields as `templateId.field` in a multi-template stream |
| A control does not send | Check Send preview, required fields, Input only, the destination, and any confirmation dialog |
| A periodic task uses an old parameter | Commit an edited project parameter. Check the current input and rejection reason in the periodic task list |
| A CAN command should edit one bit | Check the device's feedback and command layouts, then use a signal assignment. For matching layouts, an initial feedback mapping such as `HMI=IO` supplies the first payload. [CAN signal writes](uidp/can.md#发送与监视) |

Receive cards refresh in batches about every 40 ms and retain the latest rows up to their configured limit. Parsing and data bindings process incoming frames as they arrive. Auto-scroll pauses as you scroll up and resumes at the bottom. Saved projects remember format, wire display, and scroll preferences independently for Preview and an exported app. Use CAN Monitor automatic CSV recording for a persistent bus log. [Recording and retention](uidp/can.md#多通道与自动保存)

## Save, export, and upgrade

Save the `.uidp` and export an application for the current platform and architecture. The export bundles the runtime and project assets such as DBCs and images, along with the chosen theme, accent, and language. [Export and deployment](build.en.md#exported-apps)

Studio v0.9.0 reads UIDP 1.0–1.3. Project frame templates, periodic settings, and the new send options use UIDP 1.3. Check input-only and read-only settings, send routes, option tables, and record columns when opening an older project. Use the v0.9 runtime for projects that use v0.9 features and re-export standalone apps to update their bundled runtime. [Project format and migration](uidp-format.md#从旧版升级)
