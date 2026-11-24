# 取值、曲线与转换表达式

[v0.9 使用手册](../guide.zh.md) · [UIDP 工程格式](../uidp-format.md)

显示控件用 `binding` 取值。`RealtimeChart` 的每条曲线独立取值，把同一套字段写到控件的 `properties` 里，名字加 `_curve_序号_` 前缀。曲线序号从 0 开始。例如第一条曲线的 JSON 路径写 `_curve_0_jsonPath`。

在设计器的「接收与绑定」设置来源和解析模式。开关的状态判定和指示灯的亮灭值也在这里。解析和地址旁的信息按钮提供取值规则与示例。

工程数据点、显示控件和实时曲线可以通过「可视化取值」核对样例。「当前取值」可选择字段、填写 JSON 路径或按字节读取，并编辑转换表达式。选中的字段或字节范围会高亮，预览会显示样例的换算结果。在样例表中选中一项后，点击「使用所选字段」即可把它设为当前取值。

在「连接帧模板」可为来源连接选择工程模板。「帧格式」页编辑共用的分帧规则、校验和字段定义。「当前取值」页的字段、换算和手动字节设置保存到当前数据点、控件或曲线。按模板字段取值时，偏移、类型和字节序来自模板。手动取值时可设置偏移、类型、字节序及要剥除的帧头帧尾。[工程帧格式模板](connections.md#工程帧格式模板)

逐行记录使用「接收报文」时，记录列可以选择来源连接已经定义的 HEX 或 CAN 字段，也可以填写设备报文里的字段名。

Modbus 回读线圈或离散输入时直接使用位值。读取保持或输入寄存器时，设置 `mbBit`（`0`–`15`）可直接提取该位（0 或 1），设为 `-1`（默认）按所选数据类型读取整个寄存器，使用 32 位数据时还可选择寄存器序。

| `parseMode` | 取值字段 | 曲线上的写法 |
| --- | --- | --- |
| `"键值匹配"`（默认） | `key` 对应帧里的键，如 `"rpm"`。界面中的「按模板字段取值」也用此模式保存，多模板可用 `"speed.value"` | 用 `_curve_0_key` 指定数据键，图例名可以另设 |
| `"工程数据点"` | `key` 为已有工程数据点名称 | 同名加前缀 |
| `"S7"` | `s7` 对象含 `address`、`dataType`、`period`，并选择 S7 来源卡 | 在 `_curve_0_s7` 填同一对象 |
| `"JSON路径"` | `jsonPath` 用点分隔，如 `"data.sensors.0.temp"`。数组负下标从末尾数 | 同名加前缀 |
| `"HEX字节提取"` | `byteOffset` 从 0 起，`dataType` 选 `uint8/int8/uint16/int16/uint32/int32/float32`，`byteOrder` 选 `"大端(BE)"` 或 `"小端(LE)"`。可用 `frameHeader` / `frameTail` 剥去包头包尾，再从剩余数据算偏移 | 同名加前缀。普通绑定默认类型为 `uint16`，曲线为 `float32`，建议明确写设备的类型 |
| `"Modbus"` | `mbArea` 选 `"保持寄存器"`（默认）、`"输入寄存器"`、`"线圈"` 或 `"离散输入"`。`mbRegIndex` 是表内地址。`mbBit` 为 `-1`（默认，整寄存器）或 `0`–`15`（保持/输入寄存器的某一位）。`mbDataType` 选 `uint16/int16/uint32/int32/float32`，`mbRegOrder` 选 `"大端(AB CD)"` 或 `"小端(CD AB)"`。`mbSlaveFilter` 为 −1 时跟随连接卡的默认从站，`mbFcFilter` 为 −1 时按表区及地址匹配 | 地址字段写 `_curve_0_regIndex`。其余字段同名加前缀（含 `_curve_0_mbBit`）。普通绑定与各条曲线都可用 `mbPeriod` 设采集周期（0–60000 ms，0 用连接卡默认） |
| `"CAN信号"`（FluentHMI Studio） | 报文 ID、位位置、符号、缩放参数见 [CAN 信号](can.md#按位取值) | 同名加前缀 |

`Label` 把结果写入 `text`，`NumericDisplay`、`GaugeMeter`、`ProgressBar` 和 `ProgressRingDisplay` 写入 `value`。`LEDIndicator` 按 `onValue` / `offValue` 判亮灭，默认分别为 `"1"` / `"0"`。`ToggleSwitch` 写入 `checked`，`Slider` 和 `SpinBox` 写入 `value`。设备反馈可以更新这三种输入控件，只有用户操作才触发它们的发送命令。

## 转换表达式

普通绑定填 `binding.expression`，曲线填 `_curve_0_expression` 等。`value` 是刚提取的值，每条曲线在记录采样前换算一次。普通显示和曲线在表达式为空或计算失败时沿用原值。工程数据点的换算失败会标记质量，供工程公式、报警和发送判定。不同曲线可以绑定同一个键，分别换算。

| 目的 | 表达式 | 输入 → 结果 |
| --- | --- | --- |
| 温度以十分之一度上报 | `value / 10` | 253 → 25.3 |
| 读取状态字的第 1 位 | `(value >> 1) & 1` | 6 → 1 |
| 判断数值是否在区间内 | `0 < value < 40` | 25 → `True`，曲线记录 1 |

可用算术 `+ - * / // % **`、整数位运算 `& | ^ ~ << >>`、比较 `< <= > >= == !=`、`and` / `or` / `not` 和括号。比较结果在图中用 0 和 1 表示。

## 实时曲线

`curveCount` 可取 1–8，默认 1。`_curve_0_name` 是图例名。属性面板的 `curveName`、`curveExpression` 等用于编辑当前曲线，写工程时使用带序号的字段。下面是同一张图里两个系列的 `properties`，其中一条保留原始 ADC 读数，另一条计算电压。

```json
{
  "title": "ADC 采样",
  "curveCount": 2,
  "_curve_0_name": "原始读数", "_curve_0_key": "adc",
  "_curve_1_name": "电压 (V)", "_curve_1_key": "adc",
  "_curve_1_expression": "value * 3.3 / 4095"
}
```

横轴按各条曲线收到的样本计数，纵轴由可见曲线共用、自动缩放，颜色按曲线序号分配。如果运行时需要同时读清数量级差异很大的量，可以各放一个 `RealtimeChart`。

运行时可以修改「显示点数」，也可点击图例显示或隐藏某条实时曲线。已保存工程的预览和导出的应用各自恢复本机上次的点数和曲线显隐。

[完整工程模板](../uidp-format.md) · [帧解析及轮询](connections.md) · [控件属性](../uidp-widgets.md)

## 数据表格

`DataTable` 的「最新一帧」模式用「字段」「值」两列显示最近一条匹配的解析结果。要比较多条数据，选「逐行记录」，然后在「记录列」为每列填写来源字段和列标题。

下例记录 `uart1` 送来的 `temp` 和 `hum`。包含至少一个所选来源字段的消息会追加一行，缺失字段对应的单元格留空。`@time` 表示本机记录时刻。达到 `maxRows` 后，追加新行时移除最早的一行。

```json
{
  "sourceCardId": "uart1",
  "recordMode": "逐行记录",
  "recordColumns": [
    {"field": "@time", "label": "时间"},
    {"field": "temp", "label": "温度"},
    {"field": "hum", "label": "湿度"}
  ],
  "maxRows": 500
}
```

「记录来源」也可选「工程数据点」，然后在「记录列」搜索工程点或选择 `@time`。有效更新触发记录时，每行快照取各点的当前有效值。可以指定一个触发点集中记录，也可以设置固定间隔。旧值、歧义和无效值留空。对应配置为 `recordSource: "points"`、`recordTriggerPoint` 和 `recordIntervalMs`。间隔为 0 时使用触发点，触发点留空时由任一已选记录点的有效更新触发。

## 连接与子通道

[在设计器配置收发方向](../guide.zh.md#配置连接与收发方向)

直接从设备取值的绑定在 `binding.sourceCardId` 指定来源连接卡的控件 ID，曲线使用 `_curve_N_sourceCardId`。`sourceEndpoint`（曲线为 `_curve_N_sourceEndpoint`）可进一步匹配 TCP 服务端客户端、UDP 发送方的 `IP:端口` 或 MQTT 的完整 Topic。IPv6 使用 `[地址]:端口`。TCP 服务端的接收也可按对端 IP 匹配，让独占该 IP 的设备换临时端口重连后继续取值。同一 IP 下多台设备仍需可区分的子通道。各来源独立拼帧。设备数据点失去原来源时标记为旧值，等待原设备恢复。

命令用 `properties.commandTargetId` 指定发送连接，`commandEndpoint` 指定 TCP 服务端客户端、UDP 目的地址或 MQTT 发布 Topic。接收绑定和发送操作的连接留空时，会尝试自动匹配唯一连接。有多个匹配连接时请指定目标。用 `"@all"` 表示明确匹配所有兼容连接，发送时表示广播。指定连接按名称精确匹配，`*` 也是普通名称。S7 继续使用指定的 S7 卡片。单个 TCP 服务端有多个在线客户端时，发送前请选择子通道。

接收卡片、日志、数据表和 CAN 监视器通过自身的 `sourceCardId` 筛选来源，默认显示所有匹配流量。接收卡片、日志和数据表还可以设置 `sourceEndpoint`，按对端或 Topic 筛选。绑定选 `"未绑定"` 时可保留填写内容，运行时从已填完整并且启用的绑定取数。

Modbus 轮询按连接及 TCP/UDP 对端独立排队，各自使用连接卡的周期、超时和默认从站地址。TCP 服务端只轮询当前在线的客户端，UDP 可以向明确设置的对端主动发请求。某个块超时或应答异常时，只把由该块提供的绑定标为未知。MQTT 的请求 Topic 与应答 Topic 可以不同，Modbus 请求在同一 MQTT 连接内串行，轮询使用连接卡的发布 Topic。`sourceEndpoint` 筛选应答 Topic。多台 MQTT Modbus 设备应分别配置连接卡及发布 Topic。

设计器按所选连接展示子通道。更换连接后，原地址或 Topic 会退出当前配置。切换模式后若已有定向设置不适用，属性栏提示清空子通道或重新选择连接。

例如两路 CAN 都有 `IO.B4b4`：第一个控件的 `sourceCardId` 填 `CAN_A`，第二个填 `CAN_B`，各控制按钮的 `commandTargetId` 也选对应连接。ID 与信号名不负责选择通道。

工程级共用数据、跨信号公式和报警参见[工程数据与报警](alarms.md)。控件和曲线可用 `parseMode: "工程数据点"` 配合 `key` 读取命名数据点。
