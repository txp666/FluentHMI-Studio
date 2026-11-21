# Studio v0.9 教程工程 / Tutorial projects

[中文使用手册](../../docs/guide.zh.md) · [English user guide](../../docs/guide.en.md) · [UIDP 工程格式](../../docs/uidp-format.md)

从 **17** 学完整收发过程。用 **17.10** 练同一连接混收和周期停止，用 **14–16** 练报警、分页和控件风格。打开工程后进入「预览运行」。TCP 例程先连接服务端，再连接客户端。示例文档每课给出预期读数和报文。

Start with **17** for a command and feedback round trip. Use **17.10** for mixed frames and periodic stopping, or **14–16** for alarms, pages, and styles. Start Preview after opening a project. Connect the TCP server before the client. Each lab lists its expected readings or bytes.

## 17 系列 / Series 17

[中文分步实训](../../docs/tutorials/17-frame-io.zh.md) · [English lab instructions](../../docs/tutorials/17-frame-io.en.md)

打开任意 17 系列工程，按画布顶部的两步操作连接和发送。修改输入后查看对端读数，点运行预览工具栏的「工程数据监视」可核对工程数据点的来源和当前值。坏帧、拆包和主题对照样例会标明使用固定值。

Open a Series 17 project and follow the two steps on its canvas. After changing an input and sending, use **Project data monitor** in the preview toolbar to inspect current values and sources. Fixed samples are labelled for fault, frame reassembly, or topic comparison exercises.

| 工程 / Project | 练习 / What to verify |
| --- | --- |
| [17 数据收发与帧模板](17-数据收发与帧模板.uidp) | TCP、同一模板收发、必填字段、回车和按钮、设备反馈与记录 / Shared HEX template, Enter and button sends, feedback and history |
| [17.1 文本键值与 CSV](17.1-文本键值与CSV.uidp) | CRLF、键值、CSV、双周期 / Text, CSV, two independent intervals |
| [17.2 UDP 嵌套 JSON](17.2-UDP嵌套JSON.uidp) | UDP、嵌套对象、数组、布尔 / UDP, objects, arrays, Boolean values |
| [17.3 固定长度与 CRC8](17.3-固定长度与CRC8.uidp) | 固定长度、混合端序、校验验证 / Fixed length, mixed byte order, CRC8 |
| [17.4 帧尾分帧与异或](17.4-帧尾分帧与异或.uidp) | 帧尾、异或、ASCII 与 HEX 字段 / Footer, XOR, ASCII and HEX fields |
| [17.5 双字节长度与 CCITT](17.5-双字节长度与CCITT.uidp) | 两字节整帧长度、混合类型、CRC16-CCITT / Two-byte total length, mixed types, CCITT |
| [17.6 Modbus 混合寄存器](17.6-Modbus混合寄存器.uidp) | 四区轮询、FC05／06／16、寄存器范围 / Four areas, FC05, FC06, and FC16, register ranges |
| [17.7 CAN 经典帧与 FD 信号](17.7-CAN经典帧与FD信号.uidp) | 改转速、压力并回环到工程数据点，练标准帧、FD+BRS 和 RTR / Editable speed and pressure, loopback, FD and RTR · [位图实训](../../docs/tutorials/can-bit-map.zh.md) / [Bit-map tutorial](../../docs/tutorials/can-bit-map.en.md) |
| [17.8 空闲分帧与和校验](17.8-空闲分帧与和校验.uidp) | 空闲分帧、和校验、默认字段 / Idle gap, sum checksum, field defaults |
| [17.9 同连接多种 HEX 帧](17.9-同连接多种HEX帧.uidp) | 修改数据后按命令字发送，核对独立读数与拆包 / Editable values per command, separate readings, frame reassembly |
| [17.10 同连接多接收模板](17.10-同连接多接收模板.uidp) | 三模板混发、周期读取四项输入、全局取值、坏帧恢复 / Three layouts, four live inputs, shared data points, frame recovery |
| [17.11 串口收发与数据发送卡](17.11-串口收发与数据发送卡.uidp) | 虚拟串口 A/B、按字段或载荷发送、周期内改值 / Virtual serial A/B, field or payload entry, live updates during a task |
| [17.12 MQTT 发布订阅与主题取值](17.12-MQTT发布订阅与主题取值.uidp) | QoS 1、主题与相同键、当前工程值 / QoS 1, topic selection, current project values |

17–17.11 可用本机回环、内置虚拟串口或虚拟 CAN。17.12 预设公共 MQTT Broker，可以换成已有的本地 Broker。两张卡需使用同一 Broker。

Labs 17 to 17.11 use loopback or the built-in virtual links. Lab 17.12 uses a public MQTT broker. You can set both cards to the same local broker.

## 设备与界面专题 / Device and interface labs

01–08 使用串口设备或虚拟串口对端的数据。10 需要提供寄存器数据的 Modbus 从站，11 自身是从站。12 可切到虚拟 CAN，13 使用下面的 S7 模拟器，14 需要 TCP 对端发送样例。15 和 16 可以直接离线预览。

Labs 01–08 expect serial data from a device or virtual peer. Lab 10 connects to a Modbus slave and Lab 11 serves as one. Lab 12 can use Virtual CAN and Lab 13 uses the simulator below. Provide sample data from a TCP peer for Lab 14. Labs 15 and 16 run offline.

| 工程 / Project | 练习 / What to verify |
| --- | --- |
| [01 温湿度采集](01-温湿度采集.uidp) | 文本读数与实时曲线 / Text readings and trends |
| [02 温控仪监控](02-温控仪监控.uidp) | 串口 Modbus RTU、温度和设置值 / Serial Modbus RTU and temperature |
| [03 电机控制板](03-电机控制板.uidp) | HEX 数据、数值换算和控制 / HEX data, scaling, and commands |
| [04 环境监测站](04-环境监测站.uidp) | JSON 数据路径、列表与曲线 / JSON paths, displays, and charts |
| [05 AT 指令调试助手](05-AT指令调试助手.uidp) | 串口 AT 命令、手动发送和文本日志 / AT commands and text logs |
| [06 PID 调参助手](06-PID调参助手.uidp) | 参数输入与响应趋势 / Parameter inputs and trends |
| [07 ADC 采集板](07-ADC采集板.uidp) | 文本键值采样、ADC 换算和状态字 / Text samples, ADC scaling, and status bits |
| [08 电池放电测试](08-电池放电测试.uidp) | 放电控制、电压曲线和电流、累计放电量记录 / Discharge control, voltage trends, and current and capacity records |
| [10 PLC 恒压供水](10-PLC恒压供水.uidp) | Modbus 地址轮询、输出设置 / Modbus polling and write settings |
| [11 温控仪模拟器](11-温控仪模拟器.uidp) | 从站寄存器表和应答 / Slave register tables and replies |
| [12 CAN 收发测试](12-CAN收发测试.uidp) | CAN 通道、[DBC](12-CAN收发测试.dbc) 与信号 / CAN channels, DBC, and signals |
| [13 S7 PLC 联调](13-S7PLC联调.uidp) · [English project](13-S7-PLC-Monitor-en.uidp) | PLC 读数、曲线、BOOL 与 INT 写入 / S7 reads, trends, and writes |
| [14 工程数据与报警](14-工程数据与报警.uidp) | 两路来源、表达式、低电量与历史 / Two sources, expressions, alarms, and history |
| [15 分页与控件风格](15-分页与控件风格.uidp) | 四种分页导航、启动页、按钮和卡片 / Four navigation styles, the start page, buttons and cards |
| [16 输入与状态筛选](16-输入与状态筛选.uidp) | 小数输入、选择器、枚举徽标、自定义时间 / Decimal inputs, selectors, badges, and time filters |

### 06–08 和 12 的本地输入 / Local inputs for 06–08 and 12

- **06** 在虚拟串口对端依次发送 `kp=1.4,ki=0.4,kd=0.12,tgt=100,pos=82` 和 `tgt=100,pos=91`，各自以换行结束。检查三个参数读数、目标曲线和实际曲线。 / Send both lines with a newline and check the three parameter readings and the target and actual traces.
- **07** 在虚拟串口对端发送 `adc0=2048,adc1=3072,st=5`，末尾加换行，核对 ADC 换算和状态位。 / Send this line with a newline from the virtual serial peer and check the scaled ADC values and status bits.
- **08** 发送 `v=12.2,i=0.8,mah=120,run=1`，末尾加换行，核对电压曲线、记录表和放电指示。 / Send this line with a newline and check the voltage trend, the record table, and the discharge indicator.
- **12** 本机演练时，在运行预览把两张卡都切到「虚拟 CAN」，勾选 FD 再连接。确认示例工程与 DBC 在同一目录。演练 64 字节帧时，点击「发 CAN FD 报文 0x200」，核对 `FdLast` 为 `200`。 / With the project and DBC together, select **Virtual CAN** and enable **FD** on both cards before connecting. Send `0x200` and look for `FdLast` equal to `200` in the 64-byte example.

### 13 S7 模拟器 / S7 simulator

打开 [FluentPLC Simulator 工程](13-s7-first-plc.plcsim.json)，在 `127.0.0.1:1102` 启动服务。启动 HMI 预览后用机架 0、插槽 1、PG 连接。应读到 `23.5 °C`、`900 rpm` 和变化的压力。写入 `1500` 再恢复 `900`，同时检查 BOOL 邻位。

Open the simulator project and start it on `127.0.0.1:1102`. Connect from Preview with rack 0, slot 1, and PG. Expect `23.5 °C`, `900 rpm`, and a changing pressure. Write `1500` then reset to `900` and check that the adjacent BOOL bit retains its value.

[中文步骤](../../docs/tutorials/13-s7-plc.zh.md) · [English instructions](../../docs/tutorials/13-s7-plc.en.md) · [视频 / Video](https://hmi.modusignal.cn/tutorials/12-s7-plc/)

### 14–16 配置练习 / Setup exercises

- **14** 连接两路本机 TCP 服务端 `9201` 和 `9202`。给设备 A 发 `fault=0,soc=65`，给设备 B 发 `fault=0`。任一路先改为 `fault=1`，再把电量设为 `20`。在报警列表核对触发、恢复和确认。 / Connect to the two local servers, send the sample lines, then change the fault and charge values to inspect alarm transitions.
- **15** 无需连接设备。运行预览比较四种分页，返回设计器修改「当前编辑页」和「运行时启动页」。 / Compare four navigation styles offline and change the edit page and runtime start page.
- **16** 在预览比较选择器、数值框和状态徽标。用固定温度报警练习历史页的条件组合、自定义时间和导出。 / Compare input styles and use the simulated temperature alarm to filter and export history.

[工程数据、报警和布局的详细操作](../../docs/guide.zh.md#工程数据与报警) · [Data points, alarms, and layouts](../../docs/guide.en.md#project-data-and-alarms)
