# 给 AI 的 .uidp 工程模板

FluentHMI Studio **0.8** 支持 UIDP **1.0、1.1、1.2、1.3**，继续读取 0.8 之前正式版本保存的工程。显式路由和 CAN 自动保存使用 1.2。工程数据、报警、枚举、电池变色、自动恢复、小数输入、独立分段行数、CAN 发送初值映射或 ZLG 显式位时序使用 1.3。UIDP 1.2／1.3 需要 FluentHMI Studio 0.8 或更高版本。仅使用 1.0／1.1 的功能时仍保存为对应格式。这里的 0.8 是软件版本，1.x 是工程文件格式版本。


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
| 读 PLC 寄存器，或在桌面模拟从站 | [Modbus 轮询与从站](uidp/connections.md#modbus-主站与从站) | [PLC 压力监控](../examples/tutorials/10-PLC恒压供水.uidp)、[温控仪模拟器](../examples/tutorials/11-温控仪模拟器.uidp) |
| 连接经典 S7 PLC，按类型读写绝对地址 | [S7 连接、绑定与命令](s7.md) | [本地 S7 模拟器](../examples/s7-demo.uidp) |
| 按钮、输入框或滑块向设备发命令 | [命令格式与触发时机](uidp/commands.md) | [AT 指令助手](../examples/tutorials/05-AT指令调试助手.uidp) |
| 在 Studio 连接 CAN 总线，使用 DBC | [CAN 卡片、信号与命令](uidp/can.md) | [CAN 收发测试](../examples/tutorials/12-CAN收发测试.uidp)及[配套 DBC](../examples/tutorials/12-CAN收发测试.dbc) |
| 查控件属性名、默认值、范围或最小尺寸 | [Studio 控件参考](uidp-widgets.md) · [Python 版控件参考](https://raw.githubusercontent.com/txp666/PyQt-UI-Designer/main/docs/uidp-widgets.md) | 下方按类别列出控件名 |

## 分页与控件风格

Studio 的「布局容器 → 分页容器」仍保存为 `TabWidget`。点击画布页签或在属性区选择「当前编辑页」进入该页编辑。「运行时启动页」决定预览和导出应用打开时显示的页，对应工程原有的 `currentTab`。页面内容、布局和启动页设置与各自页面一起移动。删除页面时，其控件并入相邻保留页，至少保留一页。页面管理与风格切换均支持撤销。

[查看四种导航样式和设计器属性](tutorials/v0.8-guide.zh.md#分页导航)

| 属性 | 适用控件 | 可选值 |
| --- | --- | --- |
| `navigationStyle` | `TabWidget` | `经典选项卡`（缺省）、`下划线导航`、`分段导航`、`文档页签` |
| `buttonStyle` | `PushButton` | `标准`（缺省）、`强调`、`轻量`、`描边` |
| `cardStyle` | `CardContainer`、`NumericDisplay` | `标准`（缺省）、`透明`、`描边` |

四种导航共用 `tabNames`、`currentTab`、子控件 `tabIndex` 和 `_tabLayout_N`，运行时只负责切页。增加、删除、命名与排序在设计器完成。切换导航外观时，设计器保持自由布局控件相对内容区的位置，自动布局按各页设置重排，再把坐标写入工程。0.8 之前版本保存的工程省略这些风格字段时保持原有外观；控件的绑定和命令格式不变，描边按钮仍是单次点击操作。

上述可选风格由 FluentHMI Studio 支持；在其他 UIDP 运行器中打开时，外观取决于该运行器对这些属性的支持。

## 输入与状态显示

这些可选项复用原控件类型、绑定和命令；缺省字段保持 0.8 之前版本的默认行为。参见 [输入与状态筛选示例](../examples/tutorials/16-输入与状态筛选.uidp)。

| 控件 | 属性 | 作用 |
| --- | --- | --- |
| `SpinBox` | `numberMode` | `整数`（缺省）或 `小数`；小数输入需要格式 1.3 |
| `SpinBox` | `decimals`、`singleStep` | 小数模式精度 0～6（默认 2）；步长默认 1，实际步长按精度取整且至少一格 |
| `ComboBox` | `selectionStyle` | `下拉框`（缺省）、`分段`、`单选组`；共用 `items`、`currentIndex` 和 `commands` |
| `ToggleSwitch` | `switchStyle` | `开关`（缺省）、`复选框`；共用开启判定及开／关命令 |
| `NumericDisplay` | `displayStyle` | `数值`（缺省）、`状态徽标`；共用数值格式、枚举映射和卡片风格 |

数值框的范围、初值和步长可存小数；整数模式仍取整。后缀只用于显示，不发送。小数输入不会自动修改协议编码，HEX／Modbus 应选择设备支持的数据类型（如 `float32`）；发送模板不做算术换算。接收反馈、切换外观均不触发发送。

分段和单选组的接收值仍是从 0 开始的选项序号，命令按行对应；多个单选组各自独立。状态徽标适合枚举状态，映射色表示文字颜色，背景按主题生成；徽标使用组件的紧凑字号，原数值字号在切回后恢复。

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
