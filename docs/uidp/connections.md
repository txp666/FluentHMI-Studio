# 链路、帧格式与 Modbus

每个设备放一张链路卡片，运行时在卡片上选择设备并点「连接」。同类卡片可以放多张。完整显示模式请给足卡片默认高度，空间紧张时可选择 `"简洁"`。

| 卡片 | 需要设置 |
| --- | --- |
| `SerialConfigCard` | `defaultBaud`、`defaultDatabits`、`defaultParity`、`defaultStopbits` 按设备设定，波特率选项写字符串，如 `"115200"`。端口在运行时选 |
| `TcpConfigCard` | `defaultMode` 为 `"客户端"` 或 `"服务端"`，`defaultHost` 是地址，`defaultPort` 为整数。网页作为 WebSocket 客户端，需要 WebSocket 端点或转接服务 |
| `UdpConfigCard`（桌面） | `defaultLocalPort` 是本地端口，0 由系统选择。`defaultHost` / `defaultPort` 是目标地址、端口。留空目标地址时，指定非零本地端口，收到数据后可以回复最后一个发送方。目标也可填广播地址 |
| `MqttConfigCard` | `defaultHost`、`defaultPort`、`defaultSubscribe`、`defaultPublish`，可选用户名、密码和 `defaultQos`（`"0"` / `"1"` / `"2"`）。订阅支持 `+` 和 `#` 通配符。网页连接 Broker 的 WebSocket 端点 |

`serial.parser` 和 `serial.parserConfig` 是整个工程的帧格式。卡片的 `defaultParser` 为 `"项目设置"` 时直接使用它们。某张卡要单独分帧时指定解析模式和 `properties.parserConfig`。卡片的配置为空且解析模式与工程相同时沿用工程配置，其他模式使用各自默认配置。各个属性的选项和尺寸见[控件参考](../uidp-widgets.md)。

## 行文本和 JSON

`"line"` 默认 UTF-8 编码，以 LF 结束一条记录，逗号分项，等号分隔键和值。`temp=25.3,hum=60` 得到键 `temp`、`hum`。只有值的 `25.3,60` 得到 `ch0`、`ch1`。`separator`、`kvSeparator`、`terminator`、`encoding` 均可更改，编码可选 `utf-8`、`gbk`、`ascii`。记录两端会去除空白，默认也能接收 CRLF。

JSON 对象保留字段，顶层数组按 `"0"`、`"1"` 等键取值。完整的对象或数组可以跨行或连续发送，无需换行。以 `{` 或 `[` 起头的异常记录若在字符串外出现非法字节（如等号），解析器丢弃该行后继续。带 `*HH` 的 NMEA 语句先校验，合格的语句去除 `$` 和校验尾后按 CSV 解析。

MQTT 每条消息、UDP 每个数据报都自带边界。MQTT 若只收到一个数值，可直接用主题名当键，例如 `sensor/temp`。

## HEX 二进制帧

`"hex"` 下用 `parserConfig` 描述整帧。帧尾、长度字段、固定帧长、空闲超时至少配置一种。下面的工程片段让 `AA55 00 FD 55AA` 解析出键 `temp10`，其值为 253。

```json
{"parser": "hex", "parserConfig": {
  "header": "AA55", "footer": "55AA",
  "fields": [["temp10", 0, 2, "uint16", "大端(BE)"]]
}}
```

| 帧参数 | 写法 |
| --- | --- |
| `header`、`footer` | HEX 字符串，默认 `"AA55"`、`"55AA"`。空字符串表示无帧头或帧尾 |
| `lengthOffset`、`lengthSize`、`lengthEndian`、`lengthCovers` | 长度字段偏移从帧头起算，默认 −1 表示无长度字段。长度占 1 或 2 字节，端序为 `"大端(BE)"` / `"小端(LE)"`，长度覆盖 `"数据"`、`"长度字段之后"` 或 `"整帧"` |
| `fixedLength`、`idleTimeout` | 固定帧长（字节，默认 0）或等候后续数据的空闲时间（毫秒） |
| `checksum`、`checksumStart`、`checksumEndian` | 校验算法可选 `"无"`、`"和校验"`、`"异或"`、`"CRC8"`、`"CRC16-Modbus"`、`"CRC16-CCITT"`。从帧内 `checksumStart` 偏移开始校验，校验字节紧贴帧尾之前 |
| `fields` | 每项为 `[键名, 偏移, 长度, 类型]`，可加第 5 项字节序。偏移从帧头之后算，类型含 `uint8/int8/uint16/int16/uint32/int32/float32/float64/ASCII/HEX` |

`fields` 产出的键可用「键值匹配」绑定。直接选「HEX字节提取」时拿到的是整帧，控件自己的剥头、剥尾和偏移规则见[取值与曲线](data.md)。

## Modbus 主站与从站

| 模式 | 链路与设备行为 |
| --- | --- |
| `"modbus"`、`"modbus_tcp"`、`"modbus_ascii"` | 分别采用 RTU、TCP、ASCII 格式。读响应带 `slave`、`fc`，以及按响应内序号命名的 `reg0` 等寄存器或 `coil0` 等线圈。当读请求由本程序发出时，还带 `start` 和按地址命名的 `hr100`、`ir100`、`co100` 或 `di100` |
| 主站自动读取 | 卡片设置相应解析模式、控件或曲线绑定表区及地址，连接后开始轮询。`modbusPeriod` 默认 500 ms，`modbusTimeout` 默认 1000 ms，`modbusSlave` 默认 1。多张卡可轮询时，使用文档顺序里第一张符合条件且已连接的卡片 |
| 桌面模拟从站 | 串口、TCP 或 UDP 卡片设置 `modbusRole: "从站"`，帧格式选 RTU 或 TCP。`modbusSlave` 是本机地址，`modbusDelay` 是应答延时，设置 `modbusExceptions` 为 `false` 可忽略错误请求 |

桌面从站的每个区默认从地址 0 起，共 100 项，数量 0 表示该区为空。

| 表区 | 起始地址 | 数量 |
| --- | --- | --- |
| 线圈 | `slaveCoStart` | `slaveCoCount` |
| 离散输入 | `slaveDiStart` | `slaveDiCount` |
| 保持寄存器 | `slaveHrStart` | `slaveHrCount` |
| 输入寄存器 | `slaveIrStart` | `slaveIrCount` |

各项初值为 0，在 `slaveInit` 中每行写一项可设置初值，例如 `hr10=1234`。地址要落在该区配置的范围内。

绑定或曲线指定的从站地址与某张从站卡一致时，从本地表取数。主站写入后控件刷新。用户操作带 Modbus 绑定的开关、滑块、数值框可以写回原值，配置换算表达式的绑定只读。功能码 05、06、16 的写命令也可更新本地表。`ModbusSlaveTable` 显示第一张从站卡的一个区（`mbArea`），双击值可修改。

[Modbus 取值字段](data.md) · [Modbus 写命令](commands.md) · [协议示例、读块合并和总线行为](https://github.com/txp666/PyQt-UI-Designer/blob/main/docs/protocol.md)
