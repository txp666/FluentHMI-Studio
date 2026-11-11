# FluentHMI Studio 的 CAN 总线

`CanConfigCard` 连接一路 CAN。`defaultAdapter` 可选 `"PCAN"`、`"ZLG"`（另设 `zlgModel`）、`"SocketCAN"`、`"slcan"`。`defaultBitrate` 如 `"500k"`。CAN FD 使用 `canFd: true` 和数据域速率 `defaultDataBitrate`。DBC 文件写在 `dbcFile`，路径相对于工程，Studio 导出时会复制文件。

## 信号键与 DBC

有 DBC 时，`"键值匹配"` 可以取 `报文名.信号名` 或 `信号名`。优先使用带报文名的键来区分重名信号。DBC 有 `VAL_` 描述的信号还提供以 `.text` 结尾的键。没有 DBC 时可以在卡片的 `canSignals` 中每行定义一个信号，之后按信号名绑定。

```text
102 EngSpeed : 16|16@1+ (1,0) [0|8000] "rpm"
```

上例的 ID 是十六进制 `102`。`@1` 为 Intel，`@0` 为 Motorola，`+` / `-` 表示无符号 / 有符号。

## 按位取值

`parseMode` 选 `"CAN信号"` 时，`canId` 为十六进制报文 ID，`canStartBit` 为起始位，`canBitLength` 为 1–64。`canByteOrder` 可选 `"Intel"`（默认）、`"Motorola MSB"`（DBC 的 `@0`）或 `"Motorola LSB"`（起始位是最低位）。`canSigned`、`canFactor`、`canOffset` 默认分别为 `false`、1、0。

上述取值字段在绑定和曲线里的写法，以及取值后的换算，见[取值与曲线](data.md)。

## 发送与监视

`commandMode` 选 `"CAN"`，`canId` 填 DBC 报文名或 ID。在 `commandTemplate` 里写逗号分隔的 `信号=值`，`{value}` 取控件的当前值。信号值可以是十进制物理量、值描述或 `0x` 原始码。无信号的报文可按 HEX 模式组字节。`canCycle` 大于 0 时按指定毫秒周期重发，0 时使用 DBC 的 `GenMsgCycleTime`（缺省时发一次）。

下面的滑块每 100 ms 发送一次 `PC2BLDC`，向控制器写入设定转速。

```json
{"type": "Slider", "id": "speed", "geometry": {"x": 420, "y": 60, "w": 260, "h": 36},
 "properties": {"maximum": 4000, "sendOnChange": true, "commandMode": "CAN", "canId": "PC2BLDC",
                "commandTemplate": "SetMotorSpeed={value}, SetDerect=1", "canCycle": 100}}
```

`CanMonitor` 按 ID 列出收发报文。`DataSendCard` 可发 `ID#数据`，如 `102#11223344`。[可运行的 CAN 工程](../../examples/tutorials/12-CAN收发测试.uidp)和[它的 DBC](../../examples/tutorials/12-CAN收发测试.dbc)应放在同一目录。
