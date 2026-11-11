# 向设备发送命令

把 `commandMode` 和发送属性写在控件的 `properties` 中。一个控件的动作决定何时发送，所选模式决定线上字节。

| 控件 | 发什么、何时发 |
| --- | --- |
| `PushButton` | 点击发送 `command`。Modbus 模式使用 `mbValue`。选 `"05 写单线圈"` 时，`1` 或 `65280` 写开（`0xFF00`），`0` 写关（`0x0000`） |
| `ToggleSwitch` | 用户拨动时发送 `onCommand` 或 `offCommand` |
| `Slider`、`SpinBox` | `sendOnChange` 打开时发送当前数值。拖动滑块在松手时发送，键盘或滚轮每步发送。Modbus 模式下改变值也可写入 |
| `LineEdit` | `sendOnEnter` 打开时，按回车发送输入的文字 |
| `ComboBox` | `sendOnSelect` 打开时，选择一项就发送 `commands` 中对应行的命令。该行为空时发送选项文字 |

| `commandMode` | 编码 |
| --- | --- |
| `"文本"`（默认） | 将 `commandTemplate` 中的 `{value}` 替换为当前命令或值。例如 `SPEED={value}` 发出 `SPEED=75`。末尾的 `commandNewline` 可选 `"无"`、`"\\r\\n"`（默认）、`"\\n"`、`"\\r"`，这里按 JSON 文件中的转义方式书写 |
| `"HEX"` | `dataType` 为 `"HEX字符串"` 时直接使用 HEX 文本（如 `01 03 00 00`）。数值类型按 `dataType`、`byteOrder` 打包后放进模板，可以加 `frameHeader` / `frameTail` |
| `"JSON"` | `{value}` 占一个完整 JSON 值的位置。例如模板 `{"cmd":"setSpeed","value":{value}}` 可以产生 `{"cmd":"setSpeed","value":75}`。输入是合法 JSON 值时保留类型，普通文字自动转义成字符串。编码后按 `commandJsonNewline` 加结尾，默认 `"无"` |
| `"Modbus"` | `mbSlave`、`mbFunc`、`mbAddress` 指定从站、功能码和地址。按链路封装 RTU、TCP 或 ASCII。`"06 写单寄存器"` 可写 `uint16/int16`，`"16 写多个寄存器"` 还可写 `uint32/int32/float32`，32 位值占两个寄存器。`mbRegOrder` 决定寄存器序。按钮的 `mbValue` 是整数，写小数可用输入框 |
| `"CAN"`（FluentHMI Studio） | DBC 信号与原始报文的命令见 [CAN 总线](can.md) |

例如下面是一个滑块的 `properties`。用户将滑块调到 75，松手时设备收到一行 `SPEED=75`。

```json
{"minimum": 0, "maximum": 3000, "sendOnChange": true,
 "commandMode": "文本", "commandTemplate": "SPEED={value}"}
```

普通字节命令发往已连接的相应链路。MQTT 发布到 `defaultPublish`，UDP 发送数据报，两者都会保留命令的换行设置。设备需要整条消息恰为 `ON` 时，将 `commandNewline` 设为 `"无"`。

[完整工程模板](../uidp-format.md) · [链路与 Modbus](connections.md) · [控件属性与可选值](../uidp-widgets.md)
