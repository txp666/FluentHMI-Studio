# 汽车电子、电子 DIY 与嵌入式

三组控件都通过 `binding` 接收反馈。普通标量控件沿用工业控件的单位、量程和小数位。设计时显示示意值，运行时等待第一条匹配的数据。类型表列出各款的默认值。[共有属性](industrial.md#六项共有属性) · [绑定与转换表达式](data.md)

- `SteeringAngle` 和 `ServoPosition` 的转动角度按度数读取。方向盘从正上方起算，正值顺时针。舵臂在 0° 朝左、90° 朝上、180° 朝右。
- `GearPosition` 和 `TurnIndicators` 接收下表中的状态码。车辆 CAN 信号的帧 ID、字段位置、缩放和挡位编码按具体车型配置。
- `DoorStatus` 和 `UartErrors` 可以同时点亮多项。下表按位 0 起列出含义。例如四门状态值 `9` 表示左前门和右后门打开，UART 值 `5` 表示校验错误和接收溢出同时发生。设备状态字的位序可在发送端或绑定表达式中映射。
- `TirePressure` 显示单个车轮的胎压。四轮监测可放四个实例，分别设置标题和绑定键。
- `RgbLed` 接一个三位开关量，位 0、1、2 分别点亮红、绿、蓝。例如 `5` 同时点亮红蓝两色。`GpioBank` 接收一个 0–255 的整数，按位 7 到位 0 点亮，读数显示为两位十六进制。例如十进制 `165` 显示为 `0xA5`，点亮 7、5、2、0。
- `SevenSegmentDisplay` 接收一字节段码，位 0–6 对应 a–g，位 7 对应小数点，置 1 点亮。例如十进制 `109`（`0x6D`）显示 5，`191`（`0xBF`）显示带小数点的 0。八段均为低电平点亮的原始数据可用 `value ^ 255` 换算。
- `AdcChannel` 默认按 12 位 ADC 的 0–4095 显示，10 位 ADC 可将最大值设为 1023。`PwmDuty` 显示当前占空比，0% 为恒低，100% 为恒高。图中的两个周期用于表示高低电平所占比例。
- `UartRxBuffer` 接收缓冲区当前已用字节数，把 `maximum` 设为实际容量。`WirelessRssi` 直接接收 dBm 读数。`I2cAddress` 接收 7 位地址，十进制 `60` 显示为 `0x3C`。如果取到总线上的首字节，用 `value >> 1` 去掉最低位的读写标志。

回放工程按用途分页。「车身与充电」「传感与显示」「固件诊断」分别演示车门开合、数码管段码和串口错误等反馈。在工程目录运行 `python3 replay.py --frames 电子反馈.jsonl`，在桌面预览点击「本机回放」连接。网页端使用装有 PyQt5 的 Python，在同一命令末尾加 `--web`，然后连接 `ws://127.0.0.1:8765`。[电子控件回放工程](../../examples/industrial/电子控件回放.uidp) · [模拟反馈帧](../../examples/industrial/电子反馈.jsonl)

## 共享位定义与复合反馈

状态字面板 `StatusWordPanel` 显示当前各位的状态，数字时序图 `DigitalTimingChart` 显示最近 32 次匹配反馈。两者的 `properties.bitProfile` 引用工程顶层的同一份位定义。在设计器里修改一次位名、颜色或位宽，引用它的控件一起更新。下面是一段工程配置，两个控件都读取 `machine_status`。

```json
{
  "bitProfiles": {
    "machine": {"width": 8, "bits": [
      {"name": "就绪", "color": "#24895F"},
      {"name": "运行", "color": "#007CB0"}
    ]}
  },
  "widgets": [
    {"type": "StatusWordPanel", "id": "status", "geometry": {"x": 12, "y": 12, "w": 330, "h": 205},
     "properties": {"bitProfile": "machine"}, "binding": {"parseMode": "键值匹配", "key": "machine_status"}},
    {"type": "DigitalTimingChart", "id": "timing", "geometry": {"x": 356, "y": 12, "w": 380, "h": 235},
     "properties": {"bitProfile": "machine"}, "binding": {"parseMode": "键值匹配", "key": "machine_status"}}
  ]
}
```

数组从位 0 开始，位宽可设 8 或 16。未定义的位使用 `B0`、`B1` 等位名和 `#228866`。省略 `bitProfile` 时使用 `default`，工程没有该定义时按 8 位显示。时序图横轴按采样顺序排列，相同的连续值各占一个采样位置，反馈缺少绑定键时不推进，越界值留下断点。

| 控件 | 一条设备反馈 | 画面读法 |
| --- | --- | --- |
| 旋转编码器 `RotaryEncoder` | `{"encoder_count":256}` | 带符号累计脉冲数。每圈脉冲数 `properties.pulsesPerTurn` 默认为 1024，此时 256 是 0.250 圈。相邻计数从 512 降到 256 显示反向，连续相等显示停止，首个有效值只确定位置。 |
| 电芯电压矩阵 `BmsCellMatrix` | `{"cell_voltages":[3.692,3.701,3.687]}` | 数组顺序对应 C1、C2、C3。C3 最低，边框为蓝色，C2 最高，边框为橙色，压差为 14 mV。电压全部相等时所有单体同色。 |
| 直流电源面板 `DcPowerSupply` | `{"bench_supply":{"voltage":11.7,"current":1.0,"mode":"CC"}}` | 面板显示 11.70 V、1.000 A，并按设备上报点亮 CV（恒压）或 CC（恒流）。 |

电芯矩阵接收非空数字数组（V），电源面板接收含数字电压、电流和模式的对象。用键值匹配取整组数据，或用 `"parseMode":"JSON路径"` 和 `"jsonPath":"data.bench_supply"` 取嵌套字段。全景工程的「信号与诊断」页把这些读数与三位四通换向阀放在同屏，可用同一份反馈回放。[全景工程](../../examples/industrial/动态控件全景.uidp) · [回放数据](../../examples/industrial/动态反馈.jsonl)

<!-- 以下类型表由 tools/industrial_catalog.py 生成 -->

## 汽车电子

| 类型 | 默认标题 | 单位 | 量程 | 示意值 | 小数位 | 状态码 |
| --- | --- | --- | --- | --- | --- | --- |
| `VehicleSpeed` | 车速 | km/h | 0…240 | 64 | 0 | — |
| `GearPosition` | 挡位显示 | — | 0…5 | 3 | 0 | 0 P，1 R，2 N，3 D，4 S，5 L |
| `SteeringAngle` | 方向盘转角 | ° | -450…450 | -35 | 0 | — |
| `TractionBattery` | 动力电池电量 | % | 0…100 | 72 | 0 | — |
| `TurnIndicators` | 转向灯 | — | 0…3 | 1 | 0 | 0 熄灭，1 左转，2 右转，3 双闪 |
| `PedalPosition` | 踏板开度 | % | 0…100 | 35 | 0 | — |
| `DoorStatus` | 四门状态 | — | 0…15 | 9 | 0 | 0 全部关闭，位 0 左前门，位 1 右前门，位 2 左后门，位 3 右后门 |
| `TirePressure` | 胎压 | kPa | 0…400 | 235 | 0 | — |
| `FuelLevel` | 燃油余量 | % | 0…100 | 63 | 0 | — |
| `CoolantTemperature` | 冷却液温度 | °C | -40…140 | 88 | 0 | — |
| `HeadlightMode` | 前照灯模式 | — | 0…2 | 1 | 0 | 0 关闭，1 近光，2 远光 |
| `ChargeState` | 车辆充电状态 | — | 0…4 | 2 | 0 | 0 未连接，1 已连接，2 充电中，3 已充满，4 充电故障 |
| `BmsCellMatrix` | 电芯电压矩阵 | V | 电压数组 | [3.692, 3.701, …] | 3 | — |

## 电子 DIY

| 类型 | 默认标题 | 单位 | 量程 | 示意值 | 小数位 | 状态码 |
| --- | --- | --- | --- | --- | --- | --- |
| `ServoPosition` | 舵机角度 | ° | 0…180 | 65 | 0 | — |
| `Potentiometer` | 电位器位置 | % | 0…100 | 42 | 0 | — |
| `RelayModule` | 继电器模块 | — | 0…1 | 1 | 0 | 0 释放，1 吸合 |
| `RgbLed` | 三色 LED | — | 0…7 | 6 | 0 | 0 熄灭，1 红，2 绿，3 黄，4 蓝，5 品红，6 青，7 白 |
| `UltrasonicDistance` | 超声测距 | cm | 2…400 | 38.5 | 1 | — |
| `Illuminance` | 光照传感器 | lx | 0…1000 | 420 | 0 | — |
| `SevenSegmentDisplay` | 七段数码管 | — | 0…255 | 109 | 0 | — |
| `Buzzer` | 有源蜂鸣器 | — | 0…1 | 1 | 0 | 0 静音，1 鸣响 |
| `TouchPad` | 触摸按键 | — | 0…1 | 1 | 0 | 0 未触摸，1 已触摸 |
| `AirHumidity` | 空气湿度 | %RH | 0…100 | 56 | 0 | — |
| `LoadCellScale` | 电子秤 | g | 0…5000 | 128.4 | 1 | — |
| `IrBeam` | 对射光电门 | — | 0…1 | 1 | 0 | 0 光束畅通，1 光束遮挡 |
| `RotaryEncoder` | 旋转编码器 | — | 带符号整数 | 256 | 计数 0、圈数 3 | — |
| `DcPowerSupply` | 直流电源面板 | — | 电源反馈对象 | 12.00 V、0.425 A、CV | 电压 2、电流 3 | — |

## 嵌入式

| 类型 | 默认标题 | 单位 | 量程 | 示意值 | 小数位 | 状态码 |
| --- | --- | --- | --- | --- | --- | --- |
| `GpioBank` | GPIO 端口 | — | 0…255 | 165 | 0 | — |
| `AdcChannel` | ADC 采样值 | — | 0…4095 | 1536 | 0 | — |
| `PwmDuty` | PWM 占空比 | % | 0…100 | 37.5 | 1 | — |
| `CpuLoad` | CPU 负载 | % | 0…100 | 34 | 0 | — |
| `MemoryUsage` | 内存占用 | % | 0…100 | 62 | 0 | — |
| `BusStatus` | 总线状态 | — | 0…2 | 1 | 0 | 0 离线，1 在线，2 错误 |
| `ResetCause` | 复位原因 | — | 0…4 | 3 | 0 | 0 上电复位，1 外部复位，2 软件复位，3 看门狗复位，4 欠压复位 |
| `UartRxBuffer` | 串口接收缓冲区 | B | 0…256 | 96 | 0 | — |
| `UartErrors` | UART 错误标志 | — | 0…15 | 5 | 0 | 0 无错误，位 0 校验错误，位 1 帧错误，位 2 接收溢出，位 3 噪声 |
| `WirelessRssi` | 无线信号强度 | dBm | -100…-30 | -67 | 0 | — |
| `RtosTaskState` | RTOS 任务状态 | — | 0…3 | 1 | 0 | 0 就绪，1 运行，2 阻塞，3 挂起 |
| `I2cAddress` | I²C 7 位地址 | — | 0…127 | 60 | 0 | — |
| `StatusWordPanel` | 自定义状态字面板 | — | 8 或 16 位 | 165 | 0 | — |
| `DigitalTimingChart` | 数字时序图 | — | 8 或 16 位 | 165 | 0 | — |
