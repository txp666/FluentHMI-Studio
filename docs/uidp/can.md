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

`CanMonitor` 的 `displayMode` 可选 `"按 ID 汇总"`（默认）或 `"滚动日志"`。汇总模式按 ID、标准/扩展格式和收发方向合并，显示最新数据、平均周期与次数；滚动模式按到达顺序逐帧追加，并显示报文时间。切换到滚动模式时保留各汇总项的最新一帧，后续报文逐帧记录；切回汇总时合并保留的记录。

最左侧「序号」按进入监视器的收发报文累计，从 1 开始。滚动日志在淘汰旧记录、筛选或排序后保留原序号，不重新编号；汇总模式显示该 ID 最近一次更新对应的序号，与「次数」不同。切换显示模式保留编号，清空或首次 CAN 连接触发清空后重新从 1 开始。

`maxMessages` 设置最大保留条数，默认 1000，范围 1–50000。滚动模式移除最早记录，汇总模式保留最近更新的 ID。运行时也可以切换模式、修改条数上限，并通过 `autoScroll`（默认 `true`）控制是否跟随最新报文；关闭自动滚动后可翻阅历史。

点击保存图标或右键选择「导出报文」，将当前筛选和排序后的保留记录导出为 UTF-8 CSV。第一列 `sequence` 是与表内一致的整数序号；第二列 `time` 是带日期、毫秒和时区的本地时间，例如 `2026-11-17T13:14:35.843+08:00`；最后仍保留整数 `timestamp_ms` 列供程序处理。其余列包括 ID、收发方向、帧类型、长度、完整数据、周期、次数、报文名，以及开启「显示信号」时的解码值。CAN FD 的 64 字节数据完整导出；清空或超出上限移除的记录不包含在导出中。预览和独立导出应用均支持这些功能。

`DataSendCard` 可发 `ID#数据`，如 `102#11223344`。[可运行的 CAN 工程](../../examples/tutorials/12-CAN收发测试.uidp)和[它的 DBC](../../examples/tutorials/12-CAN收发测试.dbc)应放在同一目录。
