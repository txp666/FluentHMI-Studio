# UIDP 工程格式与模板

[Studio v0.9 使用手册](guide.zh.md) · [控件属性](uidp-widgets.md) · [一步一步做收发实训](tutorials/17-frame-io.zh.md)

Studio 在保存时按工程使用的功能选择 UIDP 版本。使用下表中 1.4 功能的工程，需要支持该格式的 Studio 或随包运行器。

| UIDP | 典型功能 |
| --- | --- |
| 1.0 | 基础布局、控件与常用通信配置 |
| 1.1 | 经典 S7 |
| 1.2 | 显式收发路由、早期 CAN 自动保存配置 |
| 1.3 | 工程数据和报警、工程帧模板、下发确认、组合发送、周期任务、输入和选项的新属性、扩展 CAN 设置 |
| 1.4 | 多状态小灯、工程共享枚举（含下拉选项）、仪表与进度条区间颜色、控件自定义操作 |

应用版本与文件格式各自管理。相同的 UIDP 版本可以包含多个应用版本陆续新增的可选字段。旧的 Studio、Python 版与网页编辑器按各自实现支持这些字段。

`.uidp` 是 UTF-8 JSON。FluentHMI Studio、PyQt UI Designer 和 modusignal 网页的「自定义设备」共用下面的工程模板。给 AI 这个入口和一条设备实际收发的数据即可开始，特殊协议按需查表。直接复制了本文时，相对链接以[原始文档](https://raw.githubusercontent.com/txp666/FluentHMI-Studio/main/docs/uidp-format.md)为基准。

## 一个完整的例子

设备用 115200 波特率发 `temp10=253,hum=60`，以换行结束。下面是可以直接保存为 `monitor.uidp` 的工程。连接串口后，曲线记录 25.3 °C，数值卡显示 60%，「读取」按钮发出 `READ` 并以 CRLF 结束。

```json
{
  "version": "1.0",
  "appName": "温湿度监控",
  "canvas": {"width": 800, "height": 400, "gridSize": 10},
  "serial": {"parser": "line", "parserConfig": {"separator": ","}},
  "widgets": [
    {"type": "SerialConfigCard", "id": "link", "geometry": {"x": 16, "y": 16, "w": 360, "h": 320},
     "properties": {"defaultBaud": "115200"}},
    {"type": "RealtimeChart", "id": "trend", "geometry": {"x": 392, "y": 16, "w": 392, "h": 246},
     "properties": {"title": "温度", "_curve_0_name": "温度 (°C)", "_curve_0_key": "temp10", "_curve_0_expression": "value / 10"}},
    {"type": "NumericDisplay", "id": "humidity", "geometry": {"x": 392, "y": 278, "w": 192, "h": 90},
     "properties": {"title": "湿度", "unit": "%"}, "binding": {"parseMode": "键值匹配", "key": "hum"}},
    {"type": "PushButton", "id": "read", "geometry": {"x": 608, "y": 300, "w": 152, "h": 36},
     "properties": {"text": "读取", "command": "READ"}}
  ]
}
```

## 改成自己的工程

- 在 `canvas` 设画布宽高（1–9999 的整数）。每个控件给一个全工程唯一的 `id`，`geometry` 的 `x/y/w/h` 以像素为单位。控件类型、属性和枚举值按参考中的原文填写，只写需要改的属性。`defaultBaud` 这类选项是字符串，`defaultPort` 这类数值是数字。
- `serial` 保存工程默认帧格式，串口和网络链路都能使用。每个设备放一张链路卡片，运行时在卡片上点「连接」。上例改成读其他量时，把设备发来的键填到显示控件的 `binding.key` 或曲线的 `_curve_0_key`。
- 容器用 `children` 放子控件，坐标相对于容器。`TabWidget` 的 `tabNames` 每行一个页名，子控件的 `tabIndex` 从 0 起。每页布局写 `_tabLayout_0` 等，取值为 `"自由"`、`"垂直"`、`"水平"`、`"弹性(Flex)"`。`icon` 是可选的 PNG、JPG 或 ICO 图标路径，相对于工程文件。
- 三端都能运行上面的串口模板，网页串口需支持 Web Serial 的浏览器。网页的 TCP 客户端和 MQTT 通过 WebSocket 连接。UDP、TCP 服务端、图片控件及 Modbus 从站用桌面版，CAN / CAN FD 用 FluentHMI Studio。

## 按任务查

| 需要什么 | 规则 | 可直接改的工程 |
| --- | --- | --- |
| 多信号汇总、枚举显示、实时与历史报警 | [工程数据与报警](uidp/alarms.md) | — |
| 多页切换、页面管理、按钮与卡片风格 | [分页与控件风格](#分页与控件风格) | [分页与控件风格示例](../examples/tutorials/15-分页与控件风格.uidp) |
| 显示数据、多条曲线、换算、取某一位 | [取值与曲线](uidp/data.md) | [JSON 环境监测站](../examples/tutorials/04-环境监测站.uidp) |
| 组态泵阀、料仓、流量计或压力表 | [工业设备与仪表](uidp/industrial.md) | [恒压供水泵站](../examples/industrial/恒压供水泵站.uidp)、[全控件回放](../examples/industrial/工业控件回放.uidp) |
| 多控件共用位名和颜色，显示数字时序、电芯数组或电源反馈 | [共享位定义与复合反馈](uidp/electronics.md#共享位定义与复合反馈) | [动态控件全景](../examples/industrial/动态控件全景.uidp)的「信号与诊断」页 |
| 接串口 / TCP / UDP / MQTT，设置帧头、长度、校验 | [链路与帧格式](uidp/connections.md) | [HEX 电机控制板](../examples/tutorials/03-电机控制板.uidp) |
| 用工程模板收发，或同一链路混收多种帧 | [工程帧模板及 ID 引用](uidp/connections.md#工程帧格式模板) · [按模板字段发送](uidp/commands.md#按模板字段发送) | [17 初次练习](../examples/tutorials/17-数据收发与帧模板.uidp) · [17.10 多模板混收](../examples/tutorials/17.10-同连接多接收模板.uidp) |
| 读 PLC 寄存器，或在桌面模拟从站 | [Modbus 轮询与从站](uidp/connections.md#modbus-主站与从站) | [PLC 压力监控](../examples/tutorials/10-PLC恒压供水.uidp)、[温控仪模拟器](../examples/tutorials/11-温控仪模拟器.uidp) |
| 连接经典 S7 PLC，按类型读写绝对地址 | [S7 连接、绑定与命令](s7.md) | [本地 S7 模拟器](../examples/s7-demo.uidp) |
| 向设备发命令、预览编码或增加下发确认 | [命令格式与触发时机](uidp/commands.md) | [AT 指令助手](../examples/tutorials/05-AT指令调试助手.uidp) |
| 用表格记录收到的指定字段 | [数据表格](uidp/data.md#数据表格) | |
| 在 Studio 连接 CAN 总线，使用 DBC | [CAN 卡片、信号与命令](uidp/can.md) | [CAN 收发测试](../examples/tutorials/12-CAN收发测试.uidp)及[配套 DBC](../examples/tutorials/12-CAN收发测试.dbc) |
| 查控件属性名、默认值、范围或最小尺寸 | [Studio 控件参考](uidp-widgets.md) · [Python 版控件参考](https://raw.githubusercontent.com/txp666/PyQt-UI-Designer/main/docs/uidp-widgets.md) | 下方按类别列出控件名 |

## 分页与控件风格

Studio 的「布局容器 → 分页容器」仍保存为 `TabWidget`。点击画布页签或在属性区选择「当前编辑页」进入该页编辑。「运行时启动页」决定预览和导出应用打开时显示的页，对应工程原有的 `currentTab`。页面内容、布局和启动页设置与各自页面一起移动。删除页面时，其控件并入相邻保留页，至少保留一页。页面管理与风格切换均支持撤销。

[查看四种导航样式和设计器属性](guide.zh.md#分页导航与控件外观)

| 属性 | 适用控件 | 可选值 |
| --- | --- | --- |
| `navigationStyle` | `TabWidget` | `经典选项卡`（缺省）、`下划线导航`、`分段导航`、`文档页签` |
| `buttonStyle` | `PushButton` | `标准`（缺省）、`强调`、`轻量`、`描边` |
| `cardStyle` | `CardContainer`、`NumericDisplay` | `标准`（缺省）、`透明`、`描边` |

四种导航共用 `tabNames`、`currentTab`、子控件 `tabIndex` 和 `_tabLayout_N`，运行时只负责切页。增加、删除、命名与排序在设计器完成。切换导航外观时，设计器保持自由布局控件相对内容区的位置，自动布局按各页设置重排，再把坐标写入工程。0.8 之前版本保存的工程省略这些风格字段时保持原有外观；控件的绑定和命令格式不变，描边按钮仍是单次点击操作。

上述可选风格由 FluentHMI Studio 支持；在其他 UIDP 运行器中打开时，外观取决于该运行器对这些属性的支持。

## 输入与状态显示

选择样式后，可在「操作与发送」配置下发，在「接收与绑定」配置反馈。使用示例比较不同样式。[输入与状态筛选示例](../examples/tutorials/16-输入与状态筛选.uidp)

| 控件 | 属性 | 作用 |
| --- | --- | --- |
| `SpinBox` | `numberMode` | `整数`（缺省）或 `小数`；小数输入需要格式 1.3 |
| `SpinBox` | `decimals`、`singleStep` | 小数模式精度 0～6（默认 2）；步长默认 1，实际步长按精度取整且至少一格 |
| `ComboBox` | `selectionStyle` | `下拉框`（缺省）、`分段`、`单选组`，共用 `options`、`currentIndex` 和 `sendOnSelect` |
| `ToggleSwitch` | `switchStyle` | `开关`（缺省）、`复选框`，共用状态判定及开／关命令 |
| `NumericDisplay` | `displayStyle` | `数值`（缺省）、`状态徽标`；共用数值格式、枚举映射和卡片风格 |

数值框的范围、初值和步长可存小数；整数模式仍取整。后缀只用于显示，不发送。小数输入不会自动修改协议编码，HEX／Modbus 应选择设备支持的数据类型（如 `float32`）；发送模板不做算术换算。接收反馈、切换外观均不触发发送。

使用本地选项时，三种选择样式均按从 0 开始的序号回显。引用工程枚举表时，三种样式均按设备码回显。状态徽标适合枚举状态，映射色表示文字颜色，背景按主题生成；徽标使用组件的紧凑字号，原数值字号在切回后恢复。

### 区间颜色、枚举下拉和自定义操作

仪表和进度条的 `colorRanges` 使用解析并换算后的工程值。下限包含本值，各行的下限递增。举例来说，`[{"color":"#008040"},{"min":8,"color":"#E0A000"},{"min":10,"color":"#D13438"}]` 会在 8 时切为黄色，10 时切为红色。首行省略下限可以覆盖负值。首行有下限时，更低的读数使用主题色。数据无效或过期时，读数颜色转为灰色。

下拉框的 `enumTable` 指向工程顶层 `enumTables` 中的表名。选项顺序与表中行顺序一致，例如 `10=手动`、`42=自动`。打开「选中发送」并设置文本模板 `MODE={value}`，操作员选择「自动」会发送 `MODE=42`。设备回报 42 会选中「自动」，回报未知码会清空选择。设备回读不会下发命令。

小灯和按钮支持点击（`click`），开关支持开启和关闭（`on`、`off`）。在「操作与发送」编辑 `actions`。表为空时沿用控件原本的操作。配置操作后，每次人工操作依序执行该事件的行。可选的 `condition` 用 `value` 读取本次事件值（灯的读数、按钮的 1 或开关的新状态），用 `T["点名"]` 读取有效的工程数据。条件为假或没有有效结果时跳过该行。

| `type` | 目标与参数 |
| --- | --- |
| `navigate` | `target` 为分页容器 ID，`value` 为容器内唯一的页面名称。 |
| `setParameter` | `target` 为可写工程参数名，`value` 为符合其类型的常量。 |
| `notify` | `value` 为通知正文，`target` 可填标题。 |
| `send` | 单次发送已配置的命令。按钮和开关可留空 `target` 以使用自身配置，小灯选择一个命令按钮 ID。沿用该命令的连接、编码及确认设置。确认时会再次检查条件中的工程点。成功发送后继续后面的行，取消或发送失败时结束本次执行。 |

例如，小灯的 `actions` 可填 `[{"event":"click","condition":"value == 3","type":"navigate","target":"stationPages","value":"报警"}]`。需要确认后才通知的复位按钮，可以将带条件的 `send` 行和相同条件的 `notify` 行依次放入表格。

## 从旧版升级

从 v0.8 打开旧工程后，先核对连接和控件设置，再运行预览。下发前确认默认关闭。新工程采用自动匹配唯一目标的规则，已有显式的多连接配置按工程读取。

- 输入框可按回车发送，或开启「仅输入」供按钮一次发送多个值。旧工程的只读输入框继续使用已保存的回读绑定。可编辑输入框的回读可改绑到标签或数值显示。[按钮输入列表](uidp/commands.md#一个按钮合并多个输入)
- 按钮的 HEX 和 CAN 原始数据可用 `txFrame` 设置长度与校验。单值 Modbus 和 S7 可用 `mbInputId`、`s7InputId` 引用输入框。这些设置使用 UIDP 1.3。[发送配置](uidp/commands.md#发送帧的长度与校验)
- 下拉框的旧 `items` 和 `commands` 可在「选项与发送值」中按行重新配对。新版使用 `options` 数组，每项含 `label` 和 `value`。[下拉选项示例](uidp/commands.md)
- 数据表格的旧 `columns` 可在「逐行记录 → 记录列」中重新填写。新版用 `recordColumns` 为每列指定来源字段和标题。[记录列示例](uidp/data.md#数据表格)
- 数据发送卡片原先用于输入 CAN 原始报文时，在设计器把「数据格式」设为「CAN」。使用多张连接卡的工程可显式指定收发连接和子通道。需要向所有匹配目标发送时选择「广播」。[CAN 手工发送](uidp/can.md#发送与监视) · [连接与子通道](uidp/data.md#连接与子通道)

在 v0.9.0 运行使用下发确认、周期发送或共享帧模板的工程。用新版 Studio 重新导出已有独立应用，即可更新应用随附的运行器。

引用 `ModbusConfigCard` 的早期工程会报告未知控件类型，例如 v0.2.0-rc.1 的温控仪监控和 PLC 恒压供水示例。新版通过串口或 TCP 等连接卡设置 Modbus 解析和轮询。[Modbus 连接配置](uidp/connections.md#modbus-主站与从站)

## 写完怎么检查

- **FluentHMI Studio** 运行 `FluentHMIRuntime monitor.uidp --check` 检查工程，`FluentHMIRuntime monitor.uidp --screenshot preview.png` 查看布局。`FluentHMIStudio --export out monitor.uidp` 导出独立应用。macOS 安装包内的运行器在 `FluentHMI Studio.app/Contents/Helpers/FluentHMIRuntime.app/Contents/MacOS/FluentHMIRuntime`，Windows 和 Studio 同目录，Linux 在 `/opt/FluentHMIStudio/FluentHMIRuntime`。
- **PyQt UI Designer** 在源码根目录运行 `python tools/check_uidp.py monitor.uidp` 检查属性拼写、枚举和曲线字段，再用设计器预览或导出。
- **网页** 在 [modusignal 自定义设备](https://modusignal.cn/#custom) 导入工程，切到运行并连接。没有运行器时，`python3 -m json.tool monitor.uidp` 可以先检查 JSON 语法。

## 控件一览

以下是 FluentHMI Studio 注册的类型及默认尺寸。选择控件时同时参考上文的三端支持范围。

<!-- 以下由 uidp_reference（tools/uidp_reference.cpp）从控件注册表生成，不要手改 -->

- 基础控件：`PushButton` 按钮（120×36）、`Label` 标签（120×30）、`LineEdit` 输入框（200×36）、`ComboBox` 下拉框（160×36）、`ToggleSwitch` 开关（140×30）、`Slider` 滑块（200×30）、`SpinBox` 数值框（140×36）
- 布局容器：`HBoxLayout` 水平布局（300×80）、`VBoxLayout` 垂直布局（200×200）、`FlexLayout` 弹性布局（400×200）、`GroupBox` 分组框（280×180）、`TabWidget` 分页容器（350×250）、`Separator` 分割线（200×2）、`CardContainer` 卡片容器（300×200）
- 显示控件：`LEDIndicator` LED指示灯（140×30）、`AlarmBanner` 报警栏（650×65）、`AlarmList` 报警列表（800×320）、`NumericDisplay` 数值显示（160×90）、`ProgressBar` 进度条（250×20）、`ProgressRingDisplay` 进度环（120×130）、`ImageDisplay` 图片显示（200×150）
- 图表控件：`RealtimeChart` 实时曲线（400×280）、`GaugeMeter` 仪表盘（180×180）、`DataTable` 数据表格（350×250）
- 通信控件：`S7ConfigCard` S7 连接卡片（420×240）、`SerialConfigCard` 串口配置卡片（360×320）、`TcpConfigCard` TCP 配置卡片（360×250）、`UdpConfigCard` UDP 配置卡片（360×220）、`MqttConfigCard` MQTT 配置卡片（380×390）、`CanConfigCard` CAN 配置卡片（380×270）、`DataSendCard` 数据发送卡片（350×140）、`DataRecvCard` 数据接收卡片（350×250）、`LogBox` 日志显示框（400×200）、`CanMonitor` 报文监视（560×280）、`ModbusSlaveTable` 从站寄存器表（260×300）
- [工业设备与工业仪表的类型和默认值](uidp/industrial.md)
- [汽车电子、电子 DIY 与嵌入式的类型和状态](uidp/electronics.md)
