# 取值、曲线与转换表达式

显示控件用 `binding` 取值。`RealtimeChart` 的每条曲线独立取值，把同一套字段写到控件的 `properties` 里，名字加 `_curve_序号_` 前缀。曲线序号从 0 开始。例如第一条曲线的 JSON 路径写 `_curve_0_jsonPath`。

| `parseMode` | 取值字段 | 曲线上的写法 |
| --- | --- | --- |
| `"键值匹配"`（默认） | `key` 对应帧里的键，如 `"rpm"` | 用 `_curve_0_key` 指定数据键，图例名可以另设 |
| `"JSON路径"` | `jsonPath` 用点分隔，如 `"data.sensors.0.temp"`。数组负下标从末尾数 | 同名加前缀 |
| `"HEX字节提取"` | `byteOffset` 从 0 起，`dataType` 选 `uint8/int8/uint16/int16/uint32/int32/float32`，`byteOrder` 选 `"大端(BE)"` 或 `"小端(LE)"`。可用 `frameHeader` / `frameTail` 剥去包头包尾，再从剩余数据算偏移 | 同名加前缀。普通绑定默认类型为 `uint16`，曲线为 `float32`，建议明确写设备的类型 |
| `"Modbus"` | `mbArea` 选 `"保持寄存器"`（默认）、`"输入寄存器"`、`"线圈"` 或 `"离散输入"`。`mbRegIndex` 是表内地址。`mbDataType` 选 `uint16/int16/uint32/int32/float32`，`mbRegOrder` 选 `"大端(AB CD)"` 或 `"小端(CD AB)"`。`mbSlaveFilter` 和 `mbFcFilter` 默认 −1，接受所有从站和功能码 | 地址字段例外，写 `_curve_0_regIndex`。其余取值字段同名加前缀。普通绑定可用 `mbPeriod` 设周期（0–60000 ms，0 用卡片默认），曲线用卡片周期 |
| `"CAN信号"`（FluentHMI Studio） | 报文 ID、位位置、符号、缩放参数见 [CAN 信号](can.md#按位取值) | 同名加前缀 |

`Label` 把结果写入 `text`，`NumericDisplay`、`GaugeMeter`、`ProgressBar` 和 `ProgressRingDisplay` 写入 `value`。`LEDIndicator` 按 `onValue` / `offValue` 判亮灭，默认分别为 `"1"` / `"0"`。`ToggleSwitch` 写入 `checked`，`Slider` 和 `SpinBox` 写入 `value`。设备反馈可以更新这三种输入控件，只有用户操作才触发它们的发送命令。

## 转换表达式

普通绑定填 `binding.expression`，曲线填 `_curve_0_expression` 等。`value` 是刚提取的值，每条曲线在记录采样前换算一次。表达式为空、缺省或计算失败（例如除以零），使用原值。不同曲线可以绑定同一个键，分别换算。

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

[完整工程模板](../uidp-format.md) · [帧解析及轮询](connections.md) · [控件属性](../uidp-widgets.md)
