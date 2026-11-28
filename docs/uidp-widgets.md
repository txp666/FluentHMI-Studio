# .uidp 控件参考

这份 v0.9 属性参考由控件注册表生成，列出类型、属性名、默认值、范围和选项。使用 JSON 中的原始属性名和枚举值。先看 [UIDP 工程格式](uidp-format.md) 了解布局和绑定。

修改控件元数据后，从源码根目录运行 `uidp_reference docs`。生成器同时更新本页和工程格式文档末尾的控件索引，`docs` 测试会校验两处内容。

<!-- 以下由 uidp_reference（tools/uidp_reference.cpp）从控件注册表生成，不要手改 -->

<a id="command-properties"></a>

### 命令控件共有属性

适用于 `PushButton`、`LineEdit`、`ComboBox`、`ToggleSwitch`、`Slider`、`SpinBox`。

- `commandMode`（发送模式，选项）默认 `"文本"`，可选 `"文本"` `"HEX"` `"JSON"` `"Modbus"` `"CAN"` `"S7"`
- `commandTemplate`（发送模板，文本）默认 `"{value}"`
- `commandJsonNewline`（换行符，选项）默认 `"无"`，可选 `"无"` `"\\r\\n"` `"\\n"` `"\\r"`
- `frameHeader`（帧头(HEX)，文本）默认 `""`
- `frameTail`（帧尾(HEX)，文本）默认 `""`
- `byteOrder`（字节序，选项）默认 `"大端(BE)"`，可选 `"大端(BE)"` `"小端(LE)"`
- `commandNewline`（换行符，选项）默认 `"\\r\\n"`，可选 `"无"` `"\\r\\n"` `"\\n"` `"\\r"`
- `mbSlave`（从站地址，整数）默认 `1`，范围 1–247
- `canId`（CAN 报文，文本）默认 `""`，DBC 报文名，或十六进制 ID（如 `102`、`18FF50E5`）；有信号的报文发送模板写 `信号=值`（十进制物理值，`0x` 开头为原始值），原始 ID 按 HEX 模式组字节
- `canFd`（CAN FD 帧，true/false）默认 `false`，只用于原始 ID；DBC 报文是否 FD 由 DBC 决定
- `canCycle`（循环周期(ms)，整数）默认 `0`，范围 0–60000，大于 0 时连接期间按此周期重发；0 时用 DBC 的 GenMsgCycleTime，没有则只发一次
- `commandTargetId`（发送连接，文本）默认 `""`
- `commandEndpoint`（发送子通道，文本）默认 `""`
- `s7Command`（S7 写入，对象）默认 `{"address":"DB1.DBD4","dataType":"REAL","offValue":"0","onValue":"1","value":"1"}`
- `confirmBeforeSend`（下发前确认，true/false）默认 `false`
- `showSendConfirmationDetails`（确认时显示详情，true/false）默认 `false`

<a id="link-properties"></a>

### Modbus 轮询链路卡片共有属性

适用于 `SerialConfigCard`、`TcpConfigCard`、`UdpConfigCard`、`MqttConfigCard`。

- `defaultParser`（默认解析模式，选项）默认 `"项目设置"`，可选 `"项目设置"` `"line"` `"hex"` `"modbus"` `"modbus_tcp"` `"modbus_ascii"`
- `modbusPeriod`（默认轮询(ms)，整数）默认 `500`，范围 10–60000
- `modbusTimeout`（响应超时(ms)，整数）默认 `1000`，范围 10–60000
- `modbusSlave`（默认从站，整数）默认 `1`，范围 1–247
- `parserConfig`（帧格式，对象）默认 `{}`，帧格式设置，键同工程的 serial.parserConfig；默认解析模式不是「项目设置」时生效，空对象表示沿用工程设置（模式相同时）或默认值

### 基础控件

#### `PushButton` 按钮

默认尺寸 120×36，最小 30×20；绑定数据时写入 `text`

另见[命令控件共有属性](#command-properties)。

- `text`（文本，文本）默认 `"按钮"`
- `buttonStyle`（按钮风格，选项）默认 `"标准"`，可选 `"标准"` `"强调"` `"轻量"` `"描边"`
- `enabled`（启用，true/false）默认 `true`
- `commandInputs`（输入列表，数组）默认 `[]`
- `command`（发送内容，命令）默认 `""`
- `dataType`（数据类型，选项）默认 `"HEX字符串"`，可选 `""` `"uint8"` `"int8"` `"uint16"` `"int16"` `"uint32"` `"int32"` `"float32"`
- `mbFunc`（功能码，选项）默认 `"03 读保持寄存器"`，可选 `"01 读线圈"` `"02 读离散输入"` `"03 读保持寄存器"` `"04 读输入寄存器"` `"05 写单线圈"` `"06 写单寄存器"` `"16 写多个寄存器"`
- `mbAddress`（寄存器地址，整数）默认 `0`，范围 0–65535
- `mbBit`（位号，整数）默认 `-1`，范围 -1–15
- `mbBitAction`（位操作，选项）默认 `"置位"`，可选 `"置位"` `"复位"` `"取反"`
- `mbDataType`（数据类型，选项）默认 `"uint16"`，可选 `"uint16"` `"int16"` `"uint32"` `"int32"` `"float32"`
- `mbRegOrder`（寄存器序，选项）默认 `"大端(AB CD)"`，可选 `"大端(AB CD)"` `"小端(CD AB)"`
- `mbValue`（Modbus 参数，文本）默认 `"1"`
- `notify`（按下通知，true/false）默认 `false`
- `notifyTitle`（通知标题，文本）默认 `"提示"`
- `notifyContent`（通知内容，文本）默认 `"按钮已按下"`
- `notifyType`（通知类型，选项）默认 `"成功"`，可选 `"成功"` `"信息"` `"警告"` `"错误"`
- `notifyDuration`（通知时长(ms)，整数）默认 `3000`，范围 500–30000
- `actions`（自定义操作，数组）默认 `[]`
- `sendIntervalMs`（发送间隔，整数）默认 `0`，范围 0–60000

#### `Label` 标签

默认尺寸 120×30，最小 30×20；绑定数据时写入 `text`

- `text`（文本，文本）默认 `"标签"`
- `fontSize`（字号，整数）默认 `14`，范围 8–72
- `bold`（加粗，true/false）默认 `false`
- `alignment`（对齐，选项）默认 `"Left"`，可选 `"Left"` `"Center"` `"Right"`
- `displayFormat`（显示格式，选项）默认 `"自动"`，可选 `"自动"` `"十进制"` `"十六进制(HEX)"` `"二进制(BIN)"`
- `decimals`（小数位数，选项）默认 `"自动"`，可选 `"自动"` `"0"` `"1"` `"2"` `"3"` `"4"`

#### `LineEdit` 输入框

默认尺寸 200×36，最小 30×20

另见[命令控件共有属性](#command-properties)。

- `title`（标题，文本）默认 `""`
- `placeholderText`（占位文本，文本）默认 `"请输入..."`
- `parameterName`（工程参数，文本）默认 `""`
- `text`（默认值，文本）默认 `""`
- `inputOnly`（仅输入，true/false）默认 `false`
- `dataType`（数据类型，选项）默认 `"HEX字符串"`，可选 `""` `"uint8"` `"int8"` `"uint16"` `"int16"` `"uint32"` `"int32"` `"float32"`
- `mbFunc`（功能码，选项）默认 `"06 写单寄存器"`，可选 `"03 读保持寄存器"` `"04 读输入寄存器"` `"05 写单线圈"` `"06 写单寄存器"` `"16 写多个寄存器"`
- `mbAddress`（寄存器地址，整数）默认 `0`，范围 0–65535
- `mbDataType`（数据类型，选项）默认 `"uint16"`，可选 `"uint16"` `"int16"` `"uint32"` `"int32"` `"float32"`
- `mbRegOrder`（寄存器序，选项）默认 `"大端(AB CD)"`，可选 `"大端(AB CD)"` `"小端(CD AB)"`

#### `ComboBox` 下拉框

默认尺寸 160×36，最小 30×20；绑定数据时写入 `currentIndex`

另见[命令控件共有属性](#command-properties)。

- `selectionStyle`（选择样式，选项）默认 `"下拉框"`，可选 `"下拉框"` `"分段"` `"单选组"`
- `enumTable`（工程枚举表，文本）默认 `""`
- `options`（选项与发送值，数组）默认 `[{"label":"选项1","value":""},{"label":"选项2","value":""},{"label":"选项3","value":""}]`
- `currentIndex`（默认选项，整数）默认 `0`，范围 0–99
- `sendOnSelect`（选中发送，true/false）默认 `false`
- `dataType`（数据类型，选项）默认 `"HEX字符串"`，可选 `""` `"uint8"` `"int8"` `"uint16"` `"int16"` `"uint32"` `"int32"` `"float32"`
- `mbFunc`（功能码，选项）默认 `"06 写单寄存器"`，可选 `"03 读保持寄存器"` `"04 读输入寄存器"` `"05 写单线圈"` `"06 写单寄存器"` `"16 写多个寄存器"`
- `mbAddress`（寄存器地址，整数）默认 `0`，范围 0–65535
- `mbDataType`（数据类型，选项）默认 `"uint16"`，可选 `"uint16"` `"int16"` `"uint32"` `"int32"` `"float32"`
- `mbRegOrder`（寄存器序，选项）默认 `"大端(AB CD)"`，可选 `"大端(AB CD)"` `"小端(CD AB)"`

#### `ToggleSwitch` 开关

默认尺寸 140×30，最小 30×20；绑定数据时写入 `checked`

另见[命令控件共有属性](#command-properties)。

- `switchStyle`（开关样式，选项）默认 `"开关"`，可选 `"开关"` `"复选框"`
- `label`（标签，文本）默认 `"开关"`
- `checked`（默认开启，true/false）默认 `false`
- `onCondition`（状态判定，选项）默认 `"非零为开"`，可选 `"非零为开"` `"等于指定值"`
- `onValue`（开启判定值，文本）默认 `"1"`
- `offFeedbackValue`（关闭判定值，文本）默认 `""`
- `onCommand`（开启命令，命令）默认 `""`
- `offCommand`（关闭命令，命令）默认 `""`
- `dataType`（数据类型，选项）默认 `"HEX字符串"`，可选 `""` `"uint8"` `"int8"` `"uint16"` `"int16"` `"uint32"` `"int32"` `"float32"`
- `mbArea`（写入区，选项）默认 `"线圈"`，可选 `"线圈"` `"保持寄存器"` `"保持寄存器位"`
- `mbAddress`（写入地址，整数）默认 `0`，范围 0–65535
- `mbBit`（位号，整数）默认 `0`，范围 0–15
- `notify`（切换通知，true/false）默认 `false`
- `notifyTitle`（通知标题，文本）默认 `"提示"`
- `notifyContent`（通知内容，文本）默认 `"开关已切换"`
- `notifyType`（通知类型，选项）默认 `"信息"`，可选 `"成功"` `"信息"` `"警告"` `"错误"`
- `notifyDuration`（通知时长(ms)，整数）默认 `3000`，范围 500–30000
- `actions`（自定义操作，数组）默认 `[]`

#### `Slider` 滑块

默认尺寸 200×30，最小 30×20；绑定数据时写入 `value`

另见[命令控件共有属性](#command-properties)。

- `minimum`（最小值，整数）默认 `0`，范围 -99999–99999
- `maximum`（最大值，整数）默认 `100`，范围 -99999–99999
- `value`（默认值，整数）默认 `50`，范围 -99999–99999
- `showRange`（显示上下限，true/false）默认 `false`，在滑块两端显示最小值和最大值
- `sendOnChange`（变化时向设备下发，true/false）默认 `false`
- `dataType`（数据类型，选项）默认 `"uint16"`，可选 `"uint8"` `"int8"` `"uint16"` `"int16"` `"uint32"` `"int32"` `"float32"`
- `mbFunc`（功能码，选项）默认 `"06 写单寄存器"`，可选 `"05 写单线圈"` `"06 写单寄存器"` `"16 写多个寄存器"`
- `mbAddress`（寄存器地址，整数）默认 `0`，范围 0–65535
- `mbDataType`（数据类型，选项）默认 `"uint16"`，可选 `"uint16"` `"int16"` `"uint32"` `"int32"` `"float32"`
- `mbRegOrder`（寄存器序，选项）默认 `"大端(AB CD)"`，可选 `"大端(AB CD)"` `"小端(CD AB)"`

#### `SpinBox` 数值框

默认尺寸 140×36，最小 30×20；绑定数据时写入 `value`

另见[命令控件共有属性](#command-properties)。

- `numberMode`（数值模式，选项）默认 `"整数"`，可选 `"整数"` `"小数"`
- `decimals`（小数位数，整数）默认 `2`，范围 0–6
- `minimum`（最小值，小数）默认 `0`，范围 -99999–99999
- `maximum`（最大值，小数）默认 `100`，范围 -99999–99999
- `value`（默认值，小数）默认 `0`，范围 -99999–99999
- `singleStep`（步长，小数）默认 `1`，范围 1e-06–99999
- `suffix`（后缀，文本）默认 `""`
- `sendOnChange`（变化时向设备下发，true/false）默认 `false`
- `dataType`（数据类型，选项）默认 `"uint16"`，可选 `"uint8"` `"int8"` `"uint16"` `"int16"` `"uint32"` `"int32"` `"float32"`
- `mbFunc`（功能码，选项）默认 `"06 写单寄存器"`，可选 `"05 写单线圈"` `"06 写单寄存器"` `"16 写多个寄存器"`
- `mbAddress`（寄存器地址，整数）默认 `0`，范围 0–65535
- `mbDataType`（数据类型，选项）默认 `"uint16"`，可选 `"uint16"` `"int16"` `"uint32"` `"int32"` `"float32"`
- `mbRegOrder`（寄存器序，选项）默认 `"大端(AB CD)"`，可选 `"大端(AB CD)"` `"小端(CD AB)"`

### 布局容器

#### `HBoxLayout` 水平布局

默认尺寸 300×80，最小 60×40；**容器**，子控件放在 `children` 里

- `title`（标题，文本）默认 `"水平布局"`
- `spacing`（间距，整数）默认 `8`，范围 0–100

#### `VBoxLayout` 垂直布局

默认尺寸 200×200，最小 60×40；**容器**，子控件放在 `children` 里

- `title`（标题，文本）默认 `"垂直布局"`
- `spacing`（间距，整数）默认 `8`，范围 0–100

#### `FlexLayout` 弹性布局

默认尺寸 400×200，最小 100×60；**容器**，子控件放在 `children` 里

- `title`（标题，文本）默认 `"弹性布局"`
- `direction`（方向，选项）默认 `"row"`，可选 `"row"` `"column"`
- `spacing`（间距，整数）默认 `8`，范围 0–100

#### `GroupBox` 分组框

默认尺寸 280×180，最小 80×60；**容器**，子控件放在 `children` 里

- `title`（标题，文本）默认 `"分组"`

#### `TabWidget` 分页容器

默认尺寸 350×250，最小 150×80；**容器**，子控件放在 `children` 里

- `navigationStyle`（导航风格，选项）默认 `"经典选项卡"`，可选 `"经典选项卡"` `"下划线导航"` `"分段导航"` `"文档页签"`
- `tabNames`（标签名称，多行文本）默认 `"标签1\n标签2\n标签3"`
- `currentTab`（运行时启动页，整数）默认 `0`，范围 0–9999
- `tabLayout`（当前页布局，选项）默认 `"自由"`，可选 `"自由"` `"垂直"` `"水平"` `"弹性(Flex)"`

#### `Separator` 分割线

默认尺寸 200×2，最小 20×2

- `orientation`（方向，选项）默认 `"horizontal"`，可选 `"horizontal"` `"vertical"`

#### `CardContainer` 卡片容器

默认尺寸 300×200，最小 80×60；**容器**，子控件放在 `children` 里

- `title`（标题，文本）默认 `""`
- `cardStyle`（卡片风格，选项）默认 `"标准"`，可选 `"标准"` `"透明"` `"描边"`
- `layout`（布局，选项）默认 `"自由"`，可选 `"自由"` `"垂直"` `"水平"`
- `spacing`（间距，整数）默认 `8`，范围 0–100

### 显示控件

#### `LEDIndicator` LED指示灯

默认尺寸 140×30，最小 30×20；绑定数据时写入 `on`

- `actions`（自定义操作，数组）默认 `[]`
- `label`（标签，文本）默认 `"状态"`
- `stateMode`（状态显示，选项）默认 `"开关量"`，可选 `"开关量"` `"多状态"`
- `textList`（枚举映射，多行文本）默认 `""`
- `enumTable`（工程枚举表，文本）默认 `""`
- `ledEffects`（灯效，对象）默认 `{}`
- `stateValue`（预览值，文本）默认 `"0"`
- `color`（颜色，选项）默认 `"green"`，可选 `"green"` `"red"` `"yellow"` `"blue"` `"orange"` `"cyan"` `"purple"` `"white"` `"gray"`
- `ledShape`（形状，选项）默认 `"圆形"`，可选 `"圆形"` `"圆角方形"` `"方形"` `"胶囊灯牌"`
- `ledSize`（灯珠尺寸，选项）默认 `"随控件缩放"`，可选 `"随控件缩放"` `"小"` `"标准"` `"大"` `"超大"`
- `labelPosition`（标签位置，选项）默认 `"右侧"`，可选 `"右侧"` `"底部"` `"内部居中"` `"隐藏"`
- `fontSize`（字号，整数）默认 `14`，范围 10–64
- `blink`（点亮闪烁，true/false）默认 `false`
- `on`（点亮，true/false）默认 `true`
- `onValue`（亮值，文本）默认 `"1"`
- `offValue`（灭值，文本）默认 `"0"`

#### `AlarmBanner` 报警栏

默认尺寸 650×65，最小 320×50


#### `AlarmList` 报警列表

默认尺寸 800×320，最小 480×160


#### `NumericDisplay` 数值显示

默认尺寸 160×90，最小 30×20；绑定数据时写入 `value`

- `title`（标题，文本）默认 `"温度"`
- `displayStyle`（显示样式，选项）默认 `"数值"`，可选 `"数值"` `"状态徽标"`
- `cardStyle`（卡片风格，选项）默认 `"标准"`，可选 `"标准"` `"透明"` `"描边"`
- `value`（数值，文本）默认 `"0.00"`
- `unit`（单位，文本）默认 `"°C"`
- `fontSize`（字号，整数）默认 `28`，范围 12–72
- `displayFormat`（显示格式，选项）默认 `"自动"`，可选 `"自动"` `"十进制"` `"十六进制(HEX)"` `"二进制(BIN)"` `"枚举"`
- `textList`（枚举映射，多行文本）默认 `""`
- `unknownText`（未匹配文字，文本）默认 `""`
- `enumTable`（工程枚举表，文本）默认 `""`
- `unknownColor`（未匹配颜色，颜色）默认 `"#d13438"`
- `decimals`（小数位数，选项）默认 `"自动"`，可选 `"自动"` `"0"` `"1"` `"2"` `"3"` `"4"`
- `colorRanges`（区间颜色，数组）默认 `[]`

#### `ProgressBar` 进度条

默认尺寸 250×20，最小 30×10；绑定数据时写入 `value`

- `value`（当前值，整数）默认 `50`，范围 0–99999
- `maximum`（最大值，整数）默认 `100`，范围 1–99999
- `colorRanges`（区间颜色，数组）默认 `[]`

#### `ProgressRingDisplay` 进度环

默认尺寸 120×130，最小 80×80；绑定数据时写入 `value`

- `label`（标签，文本）默认 `"进度"`
- `value`（当前值，整数）默认 `0`，范围 0–99999
- `maximum`（最大值，整数）默认 `100`，范围 1–99999

#### `ImageDisplay` 图片显示

默认尺寸 200×150，最小 30×20

- `imagePath`（图片路径，文本）默认 `""`
- `scaleMode`（缩放方式，选项）默认 `"适应"`，可选 `"适应"` `"拉伸"` `"原始大小"`，适应：保持比例放到最大；拉伸：铺满控件框；原始大小：按图片的像素大小，放不下时缩小
- `rotation`（旋转(°)，整数）默认 `0`，范围 0–359，顺时针，绕中心转；控件框不动，整张图片缩小到放得进框里
- `mirrorH`（水平镜像，true/false）默认 `false`，左右翻转，在旋转之前
- `mirrorV`（垂直镜像，true/false）默认 `false`，上下翻转，在旋转之前

### 图表控件

#### `RealtimeChart` 实时曲线

默认尺寸 400×280，最小 200×150

- `title`（标题，文本）默认 `"实时曲线"`
- `displayPoints`（显示点数，整数）默认 `200`，范围 10–10000
- `curveCount`（曲线数量，整数）默认 `1`，范围 1–8
- `currentCurve`（当前编辑曲线，整数）默认 `0`，范围 0–7
- `curveName`（曲线名称，文本）默认 `"曲线1"`
- `curveParseMode`（解析模式，选项）默认 `"键值匹配"`，可选 `"未绑定"` `"工程数据点"` `"键值匹配"` `"JSON路径"` `"HEX字节提取"` `"Modbus"` `"CAN 信号名称"` `"CAN信号"` `"S7"`
- `curveSourceCardId`（来源连接，文本）默认 `""`
- `curveSourceEndpoint`（来源子通道，文本）默认 `""`
- `curveS7`（S7 采集，对象）默认 `{"address":"DB1.DBD4","dataType":"REAL","period":0}`
- `curveKey`（匹配键，文本）默认 `""`
- `curveJsonPath`（JSON路径，文本）默认 `""`
- `curveByteOffset`（字节偏移，整数）默认 `0`，范围 0–9999
- `curveDataType`（数据类型(HEX)，选项）默认 `"float32"`，可选 `"uint8"` `"int8"` `"uint16"` `"int16"` `"uint32"` `"int32"` `"float32"`
- `curveByteOrder`（字节序(HEX)，选项）默认 `"大端(BE)"`，可选 `"大端(BE)"` `"小端(LE)"`
- `curveFrameHeader`（帧头(HEX)，文本）默认 `""`
- `curveFrameTail`（帧尾(HEX)，文本）默认 `""`
- `curveMbArea`（寄存器区，选项）默认 `"保持寄存器"`，可选 `"保持寄存器"` `"输入寄存器"` `"线圈"` `"离散输入"`
- `curveRegIndex`（寄存器地址，整数）默认 `0`，范围 0–65535
- `curveMbBit`（位号，整数）默认 `-1`，范围 -1–15
- `curveMbDataType`（数据类型(MB)，选项）默认 `"uint16"`，可选 `"uint16"` `"int16"` `"uint32"` `"int32"` `"float32"`
- `curveMbRegOrder`（寄存器序(MB)，选项）默认 `"大端(AB CD)"`，可选 `"大端(AB CD)"` `"小端(CD AB)"`
- `curveMbFcFilter`（功能码过滤，整数）默认 `-1`，范围 -1–255
- `curveMbSlaveFilter`（从站过滤，整数）默认 `-1`，范围 -1–255
- `curveMbPeriod`（采集周期(ms)，整数）默认 `0`，范围 0–60000
- `curveCanId`（CAN ID，文本）默认 `""`
- `curveCanStartBit`（起始位，整数）默认 `0`，范围 0–511
- `curveCanBitLength`（长度，整数）默认 `8`，范围 1–64
- `curveCanByteOrder`（字节序(CAN)，选项）默认 `"Intel"`，可选 `"Intel"` `"Motorola MSB"` `"Motorola LSB"`
- `curveCanSigned`（有符号，true/false）默认 `false`
- `curveCanFactor`（因子，小数）默认 `1`
- `curveCanOffset`（偏移，小数）默认 `0`
- `curveExpression`（转换表达式，文本）默认 `""`

#### `GaugeMeter` 仪表盘

默认尺寸 180×180，最小 80×80；绑定数据时写入 `value`

- `title`（标题，文本）默认 `"仪表"`
- `unit`（单位，文本）默认 `""`
- `value`（当前值，小数）默认 `0`，范围 -99999–99999
- `minimum`（最小值，小数）默认 `0`，范围 -99999–99999
- `maximum`（最大值，小数）默认 `100`，范围 -99999–99999
- `colorRanges`（区间颜色，数组）默认 `[]`

#### `DataTable` 数据表格

默认尺寸 350×250，最小 200×100

- `sourceCardId`（来源连接，文本）默认 `""`
- `sourceEndpoint`（来源子通道，文本）默认 `""`
- `recordColumns`（记录列，数组）默认 `[]`
- `recordMode`（记录方式，选项）默认 `"最新一帧"`，可选 `"最新一帧"` `"逐行记录"`
- `recordSource`（记录来源，选项）默认 `"messages"`，可选 `"messages"` `"points"`
- `recordIntervalMs`（记录间隔(ms)，整数）默认 `0`，范围 0–86400000
- `recordTriggerPoint`（触发点，文本）默认 `""`
- `maxRows`（最大行数，整数）默认 `500`，范围 10–50000

### 通信控件

#### `S7ConfigCard` S7 连接卡片

默认尺寸 420×240，最小 380×50

- `title`（标题，文本）默认 `"S7 PLC"`
- `host`（地址，文本）默认 `"127.0.0.1"`
- `port`（端口，整数）默认 `102`，范围 1–65535
- `rack`（机架，整数）默认 `0`，范围 0–7
- `slot`（插槽，整数）默认 `1`，范围 0–31
- `connectionType`（连接类型（1 PG / 2 OP / 3 Basic），整数）默认 `1`，范围 1–3
- `period`（默认轮询(ms)，整数）默认 `500`，范围 1–60000
- `timeout`（响应超时(ms)，整数）默认 `1000`，范围 1–60000
- `autoConnect`（自动连接，true/false）默认 `false`
- `autoReconnect`（自动重连，true/false）默认 `true`
- `reconnectInterval`（重连间隔(ms)，整数）默认 `1000`，范围 1–60000

#### `SerialConfigCard` 串口配置卡片

默认尺寸 360×320，最小 200×50

另见[Modbus 轮询链路卡片共有属性](#link-properties)。

- `title`（标题，文本）默认 `"串口配置"`
- `defaultPortName`（默认串口，文本）默认 `""`
- `displayMode`（显示模式，选项）默认 `"完整"`，可选 `"完整"` `"简洁"` `"单行"`
- `modbusRole`（Modbus 角色，选项）默认 `"主站"`，可选 `"主站"` `"从站"`，从站：解析模式为 modbus 或 modbus_tcp 时应答外部主站
- `modbusDelay`（响应延时(ms)，整数）默认 `0`，范围 0–5000
- `modbusExceptions`（异常响应，true/false）默认 `true`，关掉后出错的请求不应答
- `slaveCoStart`（线圈起始，整数）默认 `0`，范围 0–65535
- `slaveCoCount`（线圈数量，整数）默认 `100`，范围 0–10000
- `slaveDiStart`（离散输入起始，整数）默认 `0`，范围 0–65535
- `slaveDiCount`（离散输入数量，整数）默认 `100`，范围 0–10000
- `slaveHrStart`（保持寄存器起始，整数）默认 `0`，范围 0–65535
- `slaveHrCount`（保持寄存器数量，整数）默认 `100`，范围 0–10000
- `slaveIrStart`（输入寄存器起始，整数）默认 `0`，范围 0–65535
- `slaveIrCount`（输入寄存器数量，整数）默认 `100`，范围 0–10000
- `slaveInit`（初始值，多行文本）默认 `""`，每行一条 `区地址=值`，如 `hr100=1234`、`co3=1`；寄存器 −32768–65535，线圈、离散输入 0 或 1
- `defaultBaud`（默认波特率，选项）默认 `"115200"`，可选 `"9600"` `"19200"` `"38400"` `"57600"` `"115200"` `"230400"` `"460800"` `"921600"`
- `defaultDatabits`（默认数据位，选项）默认 `"8"`，可选 `"5"` `"6"` `"7"` `"8"`
- `defaultParity`（默认校验位，选项）默认 `"None"`，可选 `"None"` `"Even"` `"Odd"` `"Mark"` `"Space"`
- `defaultStopbits`（默认停止位，选项）默认 `"1"`，可选 `"1"` `"1.5"` `"2"`
- `defaultFlow`（默认流控制，选项）默认 `"无"`，可选 `"无"` `"RTS/CTS"` `"XON/XOFF"` `"DTR/DSR"`

#### `TcpConfigCard` TCP 配置卡片

默认尺寸 360×250，最小 240×50

另见[Modbus 轮询链路卡片共有属性](#link-properties)。

- `title`（标题，文本）默认 `"TCP 配置"`
- `displayMode`（显示模式，选项）默认 `"完整"`，可选 `"完整"` `"简洁"`
- `defaultMode`（默认模式，选项）默认 `"客户端"`，可选 `"客户端"` `"服务端"`
- `defaultHost`（默认地址，文本）默认 `"127.0.0.1"`
- `defaultPort`（默认端口，整数）默认 `8080`，范围 1–65535
- `modbusRole`（Modbus 角色，选项）默认 `"主站"`，可选 `"主站"` `"从站"`，从站：解析模式为 modbus 或 modbus_tcp 时应答外部主站
- `modbusDelay`（响应延时(ms)，整数）默认 `0`，范围 0–5000
- `modbusExceptions`（异常响应，true/false）默认 `true`，关掉后出错的请求不应答
- `slaveCoStart`（线圈起始，整数）默认 `0`，范围 0–65535
- `slaveCoCount`（线圈数量，整数）默认 `100`，范围 0–10000
- `slaveDiStart`（离散输入起始，整数）默认 `0`，范围 0–65535
- `slaveDiCount`（离散输入数量，整数）默认 `100`，范围 0–10000
- `slaveHrStart`（保持寄存器起始，整数）默认 `0`，范围 0–65535
- `slaveHrCount`（保持寄存器数量，整数）默认 `100`，范围 0–10000
- `slaveIrStart`（输入寄存器起始，整数）默认 `0`，范围 0–65535
- `slaveIrCount`（输入寄存器数量，整数）默认 `100`，范围 0–10000
- `slaveInit`（初始值，多行文本）默认 `""`，每行一条 `区地址=值`，如 `hr100=1234`、`co3=1`；寄存器 −32768–65535，线圈、离散输入 0 或 1

#### `UdpConfigCard` UDP 配置卡片

默认尺寸 360×220，最小 240×50

另见[Modbus 轮询链路卡片共有属性](#link-properties)。

- `title`（标题，文本）默认 `"UDP 配置"`
- `displayMode`（显示模式，选项）默认 `"完整"`，可选 `"完整"` `"简洁"`
- `defaultLocalPort`（默认本地端口，整数）默认 `0`，范围 0–65535，0 表示由系统选一个空闲端口
- `defaultHost`（默认远程地址，文本）默认 `"127.0.0.1"`，留空则回复最后发送方
- `defaultPort`（默认远程端口，整数）默认 `8080`，范围 1–65535
- `modbusRole`（Modbus 角色，选项）默认 `"主站"`，可选 `"主站"` `"从站"`，从站：解析模式为 modbus 或 modbus_tcp 时应答外部主站
- `modbusDelay`（响应延时(ms)，整数）默认 `0`，范围 0–5000
- `modbusExceptions`（异常响应，true/false）默认 `true`，关掉后出错的请求不应答
- `slaveCoStart`（线圈起始，整数）默认 `0`，范围 0–65535
- `slaveCoCount`（线圈数量，整数）默认 `100`，范围 0–10000
- `slaveDiStart`（离散输入起始，整数）默认 `0`，范围 0–65535
- `slaveDiCount`（离散输入数量，整数）默认 `100`，范围 0–10000
- `slaveHrStart`（保持寄存器起始，整数）默认 `0`，范围 0–65535
- `slaveHrCount`（保持寄存器数量，整数）默认 `100`，范围 0–10000
- `slaveIrStart`（输入寄存器起始，整数）默认 `0`，范围 0–65535
- `slaveIrCount`（输入寄存器数量，整数）默认 `100`，范围 0–10000
- `slaveInit`（初始值，多行文本）默认 `""`，每行一条 `区地址=值`，如 `hr100=1234`、`co3=1`；寄存器 −32768–65535，线圈、离散输入 0 或 1

#### `MqttConfigCard` MQTT 配置卡片

默认尺寸 380×390，最小 260×50

另见[Modbus 轮询链路卡片共有属性](#link-properties)。

- `title`（标题，文本）默认 `"MQTT 配置"`
- `displayMode`（显示模式，选项）默认 `"完整"`，可选 `"完整"` `"简洁"`
- `defaultHost`（默认服务器，文本）默认 `"127.0.0.1"`
- `defaultPort`（默认端口，整数）默认 `1883`，范围 1–65535
- `defaultClientId`（默认客户端ID，文本）默认 `""`
- `defaultUsername`（默认用户名，文本）默认 `""`
- `defaultPassword`（默认密码，文本）默认 `""`
- `defaultSubscribe`（默认订阅主题，文本）默认 `"device/data"`
- `defaultPublish`（默认发布主题，文本）默认 `"device/cmd"`
- `defaultQos`（默认 QoS，选项）默认 `"0"`，可选 `"0"` `"1"` `"2"`
- `defaultTls`（默认启用 TLS，true/false）默认 `false`

#### `CanConfigCard` CAN 配置卡片

默认尺寸 380×270，最小 240×50

- `title`（标题，文本）默认 `"CAN 连接"`
- `displayMode`（显示模式，选项）默认 `"完整"`，可选 `"完整"` `"简洁"`
- `defaultAdapter`（默认适配器，选项）默认 `"PCAN"`，可选 `"PCAN"` `"ZLG"` `"SocketCAN"` `"slcan"` `"虚拟 CAN"`，PCAN：Windows、macOS；ZLG：Windows；SocketCAN：Linux；slcan：所有系统
- `zlgModel`（ZLG 型号，选项）默认 `"USBCANFD-200U"`，可选 `"USBCAN-I"` `"USBCAN-I+"` `"USBCAN-I-mini"` `"USBCAN-II"` `"USBCAN-II+"` `"USBCAN-E-U"` `"USBCAN-E-mini"` `"USBCAN-2E-U"` `"USBCAN-4E-U"` `"USBCAN-8E-U"` `"USBCANFD-100U-mini"` `"USBCANFD-100U"` `"USBCANFD-200U"` `"USBCANFD-400U"` `"USBCANFD-800U"`
- `defaultDeviceIndex`（ZLG 设备索引，整数）默认 `0`，范围 0–15，同型号插了多个时的序号，从 0 开始
- `defaultChannel`（ZLG 通道，整数）默认 `0`，范围 0–7，设备上的第几路 CAN，从 0 开始
- `defaultBitrate`（默认波特率，选项）默认 `"500k"`，可选 `"10k"` `"20k"` `"50k"` `"100k"` `"125k"` `"250k"` `"500k"` `"800k"` `"1M"`
- `defaultSamplePoint`（默认采样点，选项）默认 `"87.5%"`，可选 `"87.5%"` `"81.25%"` `"80%"` `"75%"`
- `zlgSamplePoint`（ZLG 采样点，选项）默认 `"设备标准"`，可选 `"设备标准"` `"87.5%"` `"81.25%"` `"80%"` `"75%"`
- `canFd`（CAN FD，true/false）默认 `false`
- `defaultDataBitrate`（数据域波特率，选项）默认 `"2M"`，可选 `"1M"` `"2M"` `"4M"` `"5M"` `"8M"`
- `defaultDataSamplePoint`（数据域采样点，选项）默认 `"75%"`，可选 `"75%"` `"80%"`
- `canBrs`（加速 (BRS)，true/false）默认 `true`，CAN FD 报文的数据段用数据域波特率发送
- `customTiming`（自定义位时序，文本）默认 `""`，PCAN：`BRP,TSEG1,TSEG2,SJW`，FD 加 `;BRP,TSEG1,TSEG2,SJW`。时钟为经典 CAN 8 MHz、FD 80 MHz
- `zlgCustomTiming`（ZLG 自定义位时序，文本）默认 `""`，USBCAN-I/II 填 `00,1C` 或 `1,13,2,1`。留空使用设备标准或所选采样点。其他型号粘贴对应的 ZCANPRO 完整结果。4E-U 填十进制 bps
- `listenOnly`（只听模式，true/false）默认 `false`
- `autoRecover`（总线关闭自动恢复，true/false）默认 `false`
- `termination`（终端电阻，true/false）默认 `false`，ZLG USBCANFD 系列可由软件开关，其他适配器忽略
- `dbcFile`（DBC 文件，文本）默认 `""`，相对 .uidp 所在文件夹的路径；导出时复制进应用
- `canSignals`（CAN 信号，多行文本）默认 `""`，DBC 之外的信号，每行一个：`ID 名字 : 起始位|长度@1+ (因子,偏移) [最小|最大] "单位"`，ID 是十六进制，`@1` Intel、`@0` Motorola，`+` 无符号、`-` 有符号
- `canFilter`（接收过滤，文本）默认 `""`，留空收全部；否则只收这些 ID，如 `100-1FF, 18FF50E5`
- `canTxInitialState`（发送初值映射，多行文本）默认 `""`，每行 `目标报文=反馈报文` 按同名信号匹配，或 `控制报文.信号=状态报文.信号`。未收到新鲜完整反馈则拒发；留空使用原有 DBC 初值。重连清空缓存
- `canTxInitialMaxAge`（初值反馈最大间隔(ms)，整数）默认 `2000`，范围 1–60000，只在目标报文首次发送前检查反馈新鲜度；后续保留本连接已发送的控制状态

#### `DataSendCard` 数据发送卡片

默认尺寸 350×140，最小 240×100

- `title`（标题，文本）默认 `"数据发送"`
- `placeholder`（提示文字，文本）默认 `"输入要发送的数据..."`
- `commandMode`（数据格式，选项）默认 `"文本"`，可选 `"文本"` `"HEX"` `"CAN"`
- `commandNewline`（换行符，选项）默认 `"\\r\\n"`，可选 `"无"` `"\\r\\n"` `"\\n"` `"\\r"`
- `commandTargetId`（发送连接，文本）默认 `""`
- `commandEndpoint`（发送子通道，文本）默认 `""`
- `sendIntervalMs`（发送间隔，整数）默认 `0`，范围 0–60000
- `confirmBeforeSend`（下发前确认，true/false）默认 `false`
- `showSendConfirmationDetails`（确认时显示详情，true/false）默认 `false`

#### `DataRecvCard` 数据接收卡片

默认尺寸 350×250，最小 240×120

- `sourceCardId`（来源连接，文本）默认 `""`
- `sourceEndpoint`（来源子通道，文本）默认 `""`
- `title`（标题，文本）默认 `"数据接收"`
- `maxLines`（最大行数，整数）默认 `1000`，范围 50–50000

#### `LogBox` 日志显示框

默认尺寸 400×200，最小 200×80

- `sourceCardId`（来源连接，文本）默认 `""`
- `sourceEndpoint`（来源子通道，文本）默认 `""`
- `maxLines`（最大行数，整数）默认 `500`，范围 50–50000

#### `CanMonitor` 报文监视

默认尺寸 560×280，最小 240×100

- `sourceCardId`（来源连接，文本）默认 `""`
- `displayMode`（显示模式，选项）默认 `"按 ID 汇总"`，可选 `"按 ID 汇总"` `"滚动日志"`
- `maxMessages`（显示行数，整数）默认 `1000`，范围 1–50000，超出上限时移除最早记录；汇总模式按最近更新的 ID 保留
- `autoScroll`（自动滚动，true/false）默认 `true`
- `showSignals`（显示信号，true/false）默认 `false`，多一列，列出 DBC 和 canSignals 解出的信号值
- `showFilter`（显示筛选，true/false）默认 `true`，在表格上方显示按 ID 和类型筛选
- `autoSave`（自动保存，true/false）默认 `false`
- `autoSaveRows`（每文件行数，整数）默认 `100000`，范围 1–1000000
- `autoSaveDirectory`（自动保存目录，文本）默认 `""`

#### `ModbusSlaveTable` 从站寄存器表

默认尺寸 260×300，最小 160×120

- `sourceCardId`（来源连接，文本）默认 `""`
- `mbArea`（区，选项）默认 `"保持寄存器"`，可选 `"保持寄存器"` `"输入寄存器"` `"线圈"` `"离散输入"`
- `displayFormat`（显示格式，选项）默认 `"无符号"`，可选 `"无符号"` `"有符号"` `"十六进制"`，线圈和离散输入总是显示 0/1

### 工业设备与仪表

共用属性、各类型默认值和设备特有选项见[工业控件参考](uidp/industrial.md)。

### 汽车电子、电子 DIY 与嵌入式

只读控件的类型、默认值和状态见[电子控件参考](uidp/electronics.md)。
