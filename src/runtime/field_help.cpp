#include "runtime/field_help.h"

#include "runtime/app_dialog.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/StyleSheet.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/CardWidget.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QFontDatabase>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {
namespace {

QString tr(const char *text) { return QCoreApplication::translate("FieldHelp", text); }

struct HelpText
{
    QString title;
    QString body;
};

struct HelpContent
{
    QString title;
    QString introduction;
    QList<HelpText> examples;
    QList<HelpText> details;
};

// Keep translated literals on one line for tools/check_i18n.py.
// clang-format off
HelpContent contentFor(FieldHelpTopic topic)
{
    switch (topic) {
    case FieldHelpTopic::PageNavigation:
        return {tr("分页与导航"), tr("从控件库拖入「分页容器」，在「导航风格」选择经典选项卡、下划线导航、分段导航或文档页签。四种风格共用同一组页面。"), {},
            {{tr("添加与命名"), tr("点击画布页签或选择「当前编辑页」进入编辑。点击添加创建空白页。「运行时启动页」决定预览和导出应用打开时显示哪一页。")},
             {tr("删除与调整顺序"), tr("删除当前页会把其中的控件并入相邻的保留页，不删除控件。前移、后移时，页面内容与该页布局一起移动；最后一页不能删除。这些操作可撤销。")},
             {tr("切换风格"), tr("风格只改变导航外观，页面内容、数据绑定与发送设置继续保留。0.8 之前版本保存的工程仍使用原来的选项卡样式；切换时自由布局保持内容相对页顶的位置，自动布局按该页设置重新排列。")}}};
    case FieldHelpTopic::WidgetAppearance:
        return {tr("控件风格"), tr("选中控件后，在属性中选择按钮风格或卡片风格。风格跟随应用的浅色、深色和主题色。"), {},
            {{tr("按钮"), tr("标准适合普通操作，强调用于主要操作，轻量减少背景，描边突出轮廓。四种样式都保持一次点击执行一次原来的命令，描边按钮不会变成开关。")},
             {tr("卡片与数值显示"), tr("标准保留原来的卡片底色，透明只显示内容，描边保留边框。切换风格不改变控件位置、数值格式或绑定。")},
             {tr("0.8 之前的工程"), tr("未设置风格的按钮和卡片继续使用标准样式。选择新风格后，预览、保存和导出的应用使用同一设置。")}}};
    case FieldHelpTopic::SelectionStyles:
        return {tr("选择控件与开关"), tr("选择样式和开关样式只改变外观，原来的选项、数据绑定及发送设置继续使用。"),
            {{tr("模式选择"), tr("手动 → 0\n自动 → 1\n维护 → 2")}},
            {{tr("下拉、分段与单选组"), tr("在同一行填写选项名称和发送值。发送值留空时使用选项名称。默认选项按名称选择，接收绑定使用从 0 开始的选项序号。分段和单选组适合少量短选项，选项较多时使用下拉框。")},
             {tr("开关与复选框"), tr("两种样式共用状态判定和发送命令。若设备回读 1 表示开、2 表示关，可选「等于指定值」，开启判定值填 1，关闭判定值填 2。收到其他值时保留当前状态。关闭判定值留空时，其他有效数值显示为关。")},
             {tr("反馈与操作"), tr("接收数据更新选中状态时不会发送命令。只有用户操作才按发送设置执行；切换外观不会发送。")}}};
    case FieldHelpTopic::NumberInput:
        return {tr("整数与小数输入"), tr("数值框使用整数和步长 1 作为默认设置。「数值模式」选择小数后，可设置 0～6 位小数和步长。"),
            {{tr("温度设定示例"), tr("数值模式：小数\n小数位数：1\n最小值：0\n最大值：100\n默认值：23.5\n步长：0.5\n后缀：℃")}},
            {{tr("显示与编码"), tr("后缀只用于显示，不加入发送值。小数位数决定输入精度；HEX 或 Modbus 发送小数时，数据类型应选择设备支持的 float32。设备若使用整数比例值，请按协议填写整数值；发送模板只替换内容，不计算比例。小数输入不会自动改变协议的数据类型。")},
             {tr("发送时机"), tr("开启「变化时向设备下发」后，手输数值按回车或离开输入框时提交，步进按钮和滚轮按步提交。各发送模式共用此规则。关闭下发并绑定到一个本地 Modbus 从站时，控件按绑定写入本地表。")}}};
    case FieldHelpTopic::AlarmFilters:
        return {tr("报警筛选"), tr("在报警列表点击「筛选」，可以组合关键词、级别、状态和时间条件。收起筛选区仍保留当前条件，清除筛选后显示全部。"), {},
            {{tr("关键词与状态"), tr("关键词匹配报警名称或文字。当前报警按活动、未确认、已恢复待确认或数据未知等状态筛选。历史记录按触发、恢复、确认和消音事件筛选。")},
             {tr("时间范围"), tr("当前报警按触发时间，历史记录按事件时间筛选。自定义起止时间使用本机时间，包含两个端点；不限时间时也能显示尚未收到数据的报警。")},
             {tr("日期与时间输入"), tr("自定义范围中，点击日历图标选日期，点击时间图标选择时、分、秒并确认；取消不修改时间。也可直接用键盘编辑两个输入框，按 Enter 或移开焦点后生效。")},
             {tr("确认与导出"), tr("当前报警页提供确认和消音。历史记录页可以导出筛选结果或全部历史。")}}};
    case FieldHelpTopic::CanRecovery:
        return {tr("总线关闭自动恢复"), tr("默认关闭。开启后，PCAN 使用驱动自动恢复；ZLG 在本通道线程中每秒尝试复位并重新启动，不关闭其他通道。"), {},
            {{tr("适用情况"), tr("仅针对控制器 Bus-off 状态，不处理 USB 拔出或设备失联。若接线、终端电阻或波特率仍有问题，恢复后可能再次 Bus-off。")},
             {tr("SocketCAN"), tr("需要按卡片提示执行带 restart-ms 1000 的系统配置命令；本程序不修改系统网络配置。slcan 不提供自动恢复开关。")},
             {tr("设备识别"), tr("PCAN 列出驱动报告的通道。ZLG 搜索已安装驱动提供的 USB 型号名称；兼容卡未提供明确型号时请手动选择，同型号多台设备仍需核对序号。")}}};
    case FieldHelpTopic::EnumDisplay:
        return {tr("枚举显示"), tr("点「编辑枚举」设置每个值的文字和颜色。另存为工程枚举后，小灯和徽标可以共用同一张表。"),
            {{tr("状态映射"), QStringLiteral("0=停止\n1=运行|#107c10\n2=故障|#d13438")}}, // i18n: ignore
            {{tr("灯效"), tr("小灯选择「多状态」，在每行设置熄灭、常亮、慢闪或快闪。输入预览值可查看灯效。")},
             {tr("未匹配值"), tr("没有匹配时显示原始值，或填写「未匹配文字」；「未匹配颜色」留空使用默认颜色。")},
             {tr("状态徽标"), tr("数值显示的「显示样式」选择「状态徽标」，再把显示格式设为枚举，即可显示紧凑的状态文字。映射颜色仍表示文字颜色，徽标背景随主题生成浅色衬底；枚举模式不显示单位。")}}};
    case FieldHelpTopic::ValueThreshold:
        return {tr("电量分段变色"), tr("开启后按当前百分比选择颜色，0.8 之前版本保存的工程默认关闭。"), {},
            {{tr("示例"), tr("低电量上限 20，预警上限 40：≤20% 红色，20%～40% 黄色，>40% 绿色。两个边界都计入较低区间，可自行设置颜色。")}}};
    case FieldHelpTopic::ProjectLogic:
        return {tr("工程数据与报警"), tr("点击设计器顶部「数据与报警」，或在工程属性打开「工程数据与报警」。控件、曲线选择「工程数据点」，再从绑定字段下拉框选择或搜索点名。"),
            {{tr("任一路故障触发总故障"), QStringLiteral("T[\"L1\"] or T[\"L2\"] or T[\"L3\"]")},
             {tr("温度报警条件"), QStringLiteral("T[\"温度\"] > 80")}}, // i18n: ignore
            {{tr("数据点"), tr("接收数据点选择来源连接和地址；计算表达式可引用多个已定义的数据点。不同连接使用不同名称。Modbus 和 S7 数据点在后台持续采集。")},
             {tr("工程参数"), tr("绑定工程参数的输入框在按回车或离开字段时提交。周期任务每轮读取已提交的有效值。")},
             {tr("重命名"), tr("双击数据点名称修改，计算表达式、报警条件、控件和曲线绑定会同步更新。点击确定一起保存，取消不会改变工程；也可一次撤销整次修改。设备原始字段名和普通显示文字不随点名改变。")},
             {tr("故障与未知"), tr("工程表达式的 and/or 返回布尔结果。任一路已知故障，or 汇总就为真；正常与未知做 or 时仍为未知，不误报恢复。and 有已知假值时为假。未知输入不当作零参与算术计算。")},
             {tr("报警"), tr("级别 1～3，3 最高。条件从假变真时触发，从真变假时恢复；确认不会修改设备，恢复后未确认的报警仍保留。消音只停止本次提醒，再次触发会恢复提醒。")},
             {tr("历史"), tr("最近 2000 条触发、恢复、确认和消音记录保存在本机，可在报警列表切换查看、导出。历史按工程标识区分；没有报警定义的工程不会创建记录文件。")}}};
    case FieldHelpTopic::IndustrialTextLayout:
        return {
            tr("工业组件文字定位"),
            tr("「显示标题」「显示读值」分别控制标题和数值文字；隐藏文字会保留原设置和数据绑定。单位仍可用于图形内的标注。"),
            {},
            {{tr("图形周围"), tr("标题和读值紧邻图形排列，放大组件时保持紧凑。适合仪表、电池等独立显示组件。")},
             {tr("中心偏移"), tr("以组件中心为原点，水平偏移向右为正，垂直偏移向下为正，单位为设计画布像素。设置后保持相对于中心的偏移；调整组件大小不会按比例拉大文字与中心的距离。")},
             {tr("示例：在电池中间显示读值"), tr("关闭「显示标题」，开启「显示读值」，文字定位选择「中心偏移」，读值水平偏移和垂直偏移都填 0。需要避开图形细节时，再小幅调整偏移。")},
             {tr("悬浮读值"), tr("需要鼠标悬停时查看额外读值信息，可开启「悬浮读值」；默认关闭，避免遮挡相邻组件。")}}};
    case FieldHelpTopic::DataBinding:
        return {
            tr("接收数据与控件取值"),
            tr("「接收与绑定」决定控件如何取值和更新显示。「操作与发送」决定点击、拨动或修改控件时发什么。曲线的解析模式只作用于当前编辑的曲线。"),
            {{tr("键值匹配：收到 temperature=25.3 时，绑定字段填"), QStringLiteral("temperature")},
             {tr("JSON 路径：从 data.sensors 数组第一项读取 temp"), QStringLiteral("data.sensors.0.temp")}},
            {{tr("如何选择解析模式"),
              tr("文本键值和已解析的 HEX 字段使用「键值匹配」。嵌套 JSON 使用「JSON路径」，数组下标从 0 开始。用「CAN 信号名称」在位图中选择 DBC 或内联信号。用「CAN 按位取值」在位图中设置报文 ID 和位域，也可粘贴样例报文查看解析值。")},
             {tr("HEX 字节偏移"),
              tr("「HEX字节提取」先剥去绑定中填写的帧头和帧尾，再从剩余数据的第 0 字节开始计数。例如 AA 55 00 FD 55 AA，帧头填 AA 55、帧尾填 55 AA，偏移 0、uint16、大端得到 253。字节序和数据类型必须与设备一致。")},
             {tr("没有刷新时先检查"),
              tr("先确认连接卡片收到完整报文，再核对取值方式和字段名。修改控件标题不会改变绑定字段。Modbus 和 S7 还需要正确的表区、地址和采集设置，相关地址旁有专门说明。")}}};
    case FieldHelpTopic::TransformExpression:
        return {
            tr("转换表达式"),
            tr("value 表示当前绑定刚提取的值。表达式在显示或记录曲线前执行，常用于单位换算、取状态位和阈值判断；留空时直接使用原值。"),
            {{tr("十分之一单位：253 显示为 25.3"), QStringLiteral("value * 0.1")},
             {tr("读取状态字第 4 位：位号从 0 开始，输入需为整数"), QStringLiteral("(value >> 4) & 1")},
             {tr("阈值判断：大于 80 时为真"), QStringLiteral("value > 80")}},
            {{tr("支持的运算"),
              tr("可用 + - * / // % **、整数位运算 & | ^ ~ << >>、比较、and / or / not 和括号。比较产生 True 或 False；曲线将其记录为 1 或 0。绑定转换表达式处理当前 value；跨信号计算请在「工程数据与报警」定义计算数据点，用 T[\"名称\"] 引用。表达式不能调用函数。")},
             {tr("计算失败与发送"),
              tr("计算失败（如除以零、对小数做位运算）时使用原值。这里的换算只处理接收值，不会自动对发送值做反向换算；发送模板、写入地址和值应单独配置。")}}};
    case FieldHelpTopic::CommandTemplates:
        return {
            tr("发送模式与模板"),
            tr("发送模式决定命令如何编码；{value} 由控件本次的命令或数值替换。按钮使用发送内容，开关使用开启/关闭命令，滑块和数值框使用当前数值。"),
            {{tr("文本模板：数值 75 生成 SPEED=75"), QStringLiteral("SPEED={value}")},
             {tr("JSON 模板：{value} 占一个完整 JSON 值的位置"), QStringLiteral("{\"cmd\":\"setSpeed\",\"value\":{value}}")}},
            {{tr("文本和 JSON 的结尾"),
              tr("文本默认追加回车换行；JSON 默认不追加换行。设备要求只发送 ON 时，发送内容填 ON、模板填 {value}、换行符选「无」。JSON 的 {value} 外不加引号，合法数值保留数值类型，普通文字会自动转义为字符串。")},
             {tr("HEX 的两种输入"),
              tr("数据类型选「HEX字符串」时，发送内容填写 01 03 等十六进制字节。选择 uint16 等数值类型时，输入十进制数，再按字节序打包；例如 258、uint16、大端生成 01 02。帧头和帧尾另填 HEX 字节。")},
             {tr("一个按钮发送多个输入值"),
              tr("从按钮的「编辑发送配置」新建输入框或选择已有的。文本、JSON、HEX 和 CAN 可自动生成模板，也可用 {p1}、{p2} 引用各项，例如 KP={p1},KI={p2}。HEX 和 CAN 原始数据在列表中逐项选数据类型。下发确认会显示本次使用的输入值。")},
             {tr("长度与校验"),
              tr("在 HEX 或 CAN 原始数据的发送配置中，可以检查固定帧长，或在指定偏移插入长度字段。校验结果写在数据之后。以帧头 AA 55、数据 10 22 30、帧尾 0D 为例，偏移 2 写入 1 字节数据长度，从偏移 3 算异或，发送 AA 55 03 10 22 30 02 0D。")},
             {tr("Modbus、CAN 和 S7"),
              tr("Modbus FC16 按列表顺序写连续寄存器。其他 Modbus 功能按功能码使用一项读取数量或写入值，S7 按绝对地址写一个值，均可从现有输入框取值。带信号定义的 CAN 报文使用信号名赋值，例如 Speed={value}。")},
             {tr("何时发送"),
              tr("按钮点击、开关拨动和输入框按回车时发送。下拉框选中发送，滑块和数值框提交变化时发送，由各控件的下发开关启用。设备回读只更新控件显示。")}}};
    case FieldHelpTopic::ModbusAddressing:
        return {
            tr("Modbus 地址与采集"),
            tr("先选择寄存器区，再填写从 0 开始的协议地址。设备手册中的 40001 若表示第一个保持寄存器，界面应选「保持寄存器」、地址填 0；请按手册确认基准，不要直接把 40001 当偏移。"),
            {{tr("模拟从站：填入连接卡片的「初始值」，地址须在已配置表区内"), QStringLiteral("hr10=1234\nco3=1")}},
            {{tr("读取、写入分别配置"),
              tr("在「操作与发送」配置写入区和地址，在「接收与绑定」配置读取区和地址。若同一从站的保持寄存器 10 用于控制和回读，两处均选保持寄存器、地址 10。开关写保持寄存器时用 FC06 写 1 或 0，选「保持寄存器位」或为按钮的 FC06 设置位号（0–15）时，先用 FC03 读出再只改目标位写回，线圈使用 FC05。「发送预览」可查看具体命令。")},
             {tr("32 位数与寄存器序"),
              tr("uint16/int16 占 1 个寄存器；uint32/int32/float32 占连续 2 个。大端(AB CD) 先高字，小端(CD AB) 先低字，不改变每个寄存器内部的字节序。发送 32 位值使用「16 写多个寄存器」。")},
             {tr("主站自动采集"),
              tr("连接卡片选择对应的 Modbus 解析模式后，按控件或曲线的绑定自动读取。绑定的采集周期 0 使用连接卡片默认轮询周期；从站过滤 -1 接受所有从站，轮询时使用卡片默认从站。功能码过滤 -1 表示不过滤，并不是停止采集。")},
             {tr("模拟从站角色"),
              tr("串口、TCP、UDP 卡片可作为本机模拟从站，应答外部主站的 RTU 或 TCP 请求；它不会按主站方式主动轮询。hr、ir、co、di 分别表示保持寄存器、输入寄存器、线圈、离散输入的初值。")}}};
    case FieldHelpTopic::S7Addressing:
        return {
            tr("S7 地址与连接"),
            tr("接收绑定选择「S7 来源连接」，发送属性选择「S7 目标连接」，引用的是工程中 S7 卡片的控件 ID。地址使用绝对地址，并选择匹配宽度的数据类型。"),
            {{tr("DB1 中从字节 4 开始的 32 位浮点数：类型选 REAL"), QStringLiteral("DB1.DBD4")},
             {tr("DB1 第 0 字节的第 1 位：类型选 BOOL"), QStringLiteral("DB1.DBX0.1")}},
            {{tr("地址宽度与采集周期"),
              tr("DBX 对应 BOOL，DBB 对应 BYTE/SINT，DBW 对应 WORD/INT，DBD 对应 DWORD/DINT/REAL。也支持 M、I、Q 区的位、B、W、D 地址；I 区只读。采集周期 0 使用所选连接卡片的默认轮询周期。")},
             {tr("连接参数"),
              tr("地址、端口、机架和插槽按 PLC 配置填写；默认端口为 102，连接类型 1=PG、2=OP、3=S7 Basic。当前使用 Classic S7 绝对地址访问，设备需允许相应访问；不支持符号地址或优化 DB。")},
             {tr("开关的判定与写入"),
              tr("「接收与绑定」中的「状态判定」决定回读如何更新开关显示。S7 的「开启写入值」「关闭写入值」指定了拨动时的值。BOOL 写入只更新目标位。")}}};
    case FieldHelpTopic::ReceiveFraming:
        return {
            tr("接收分帧与字段"),
            tr("「跟随工程」显示生效的工程解析模式，并使用工程的帧格式。继承 Modbus 时仍可设置卡片自己的轮询或从站参数。"),
            {{tr("line：行结束符填写反斜杠 n"), QStringLiteral("\\n")},
             {tr("hex：字段表每行一个，省略字节序时使用大端"), QStringLiteral("temperature,0,2,uint16")}},
            {{tr("行文本何时更新"),
              tr("line 模式默认以换行结束一条文本记录，逗号分项、等号分隔键和值。设备没有发出行结束符时会继续等候。完整 JSON 对象或数组按括号边界解析，无需换行；MQTT 消息、UDP 数据报有自己的边界。")},
             {tr("行结束符"), tr("输入 \\n 表示换行，\\r 表示回车，\\t 表示制表符，\\\\ 表示反斜杠。")},
             {tr("二进制帧如何结束"),
              tr("hex 模式至少配置帧尾、长度字段、固定帧长、空闲超时中的一种。固定帧长包含帧头和帧尾；长度字段偏移从整帧第 0 字节算起。空闲超时表示等到连续一段时间没有新字节才交付帧，并非固定刷新周期。")},
             {tr("长度字段偏移"), tr("长度字段偏移为 -1 时跳过长度字段。固定帧长为 0 时，按其他结束条件分帧。")},
             {tr("字段偏移从哪里算"),
              tr("字段表格式为 名字,偏移,长度,类型[,字节序]，偏移从帧头之后的第 0 字节算起。帧头 AA55、帧尾 55AA、数据 AA55 00FD 55AA，配合上面的字段表得到 temperature=253；控件用「键值匹配」绑定 temperature。")},
             {tr("接收帧预览"),
              tr("在「示例报文」打开「解析预览」，粘贴一帧 HEX 检查接收配置。点选解析字段可在字节布局中高亮对应范围，视图中的偏移从帧首算起。")},
             {tr("工程和卡片的设置关系"),
              tr("卡片选择「项目设置」时沿用工程配置。独立模式下，卡片帧格式为空且模式与工程相同时沿用工程配置，否则使用该模式默认值。连接参数和帧格式修改后重新连接才用于新链路。")}}};
    case FieldHelpTopic::CanInitialState:
        return {
            tr("发送初值映射"),
            tr("在设计器中选中 CAN 连接卡片，将映射填入「发送初值映射」，保存工程后重新连接。左侧是要发送的控制报文，右侧是设备发回的状态报文。"),
            {{tr("同名信号：填写到连接卡片"), QStringLiteral("HMI=IO")},
             {tr("不同信号名：每行指定一对字段"),
              QStringLiteral("Control.Red=Status.RedFeedback\nControl.Green=Status.GreenFeedback")}},
            {{tr("与开关发送模板的关系"),
              tr("以 640S 的 LED_RED 为例：连接卡片填 HMI=IO；开关的发送模式选 CAN，CAN 报文填 HMI，发送模板填 B4b4={value}，开启命令填 1，关闭命令填 0。映射用于准备其他输出的初值，模板指定本次要修改的信号。")},
             {tr("何时使用反馈"),
              tr("首次按信号发送前，使用最近收到的有效反馈初始化映射字段，再执行本次信号赋值。后续发送沿用本连接中 HMI 已发送的状态；重连后重新等待反馈。它不会在每次接收时覆盖发送缓存。")},
             {tr("反馈必须满足什么条件"),
              tr("必须收到包含全部映射字段的有效报文，且未超过「初值反馈最大间隔」（默认 2000 ms）。没有反馈、反馈过期或被接收过滤排除时拒绝首次发送。设备已周期广播状态时直接接收即可。")},
             {tr("哪些位会被复制"), tr("报文映射只复制同名信号；字段映射只复制明确指定的信号。位长、符号、数值类型、比例、偏移和单位须一致，位位置和字节序可不同。未映射字段使用控制报文的 DBC 初值（未定义时为 0）。请确认反馈和控制表示同一状态。")},
             {tr("留空时"), tr("使用原有行为：首次发送从 DBC 初值开始，后续保留 HMI 已发送的其他位。接收绑定只用于显示，不会自动建立发送初值映射。")}}};
    case FieldHelpTopic::CanSignalCommand:
        return {
            tr("CAN 信号发送"),
            tr("发送模式选 CAN；「CAN 报文」填写 DBC 报文名或十六进制 ID。有信号定义时，「发送模板」填写 信号=值；{value} 会替换成控件本次要发送的值。"),
            {{tr("LED_RED 的发送模板"), QStringLiteral("B4b4={value}")},
             {tr("一次设置多个信号"), QStringLiteral("Enable=1, Speed={value}")}},
            {{tr("LED_RED 的完整设置"),
              tr("对于 640S 示例：CAN 报文填 HMI；开启命令填 1，关闭命令填 0；发送模板使用上面的 B4b4={value}。点击开启会赋值 B4b4=1，关闭会赋值 B4b4=0。其他工程应替换成自己的报文名和信号名。")},
             {tr("其他位从哪里来"),
              tr("按信号赋值会在发送基准上修改指定字段，最终仍发送完整 CAN 帧。若需用设备反馈初始化基准，在 CAN 连接卡片上配置「发送初值映射」。不要把 HMI=IO 填进发送模板。")},
             {tr("数值与字节序"),
              tr("十进制数按信号的物理值编码，也可填写 DBC 值描述；0x 开头表示原始码。有信号定义时使用信号自己的位位置和字节序。仅填写 {value} 不能指定要写哪个信号。")},
             {tr("循环周期"), tr("大于 0 时按指定毫秒数重复发送；0 时使用 DBC 的 GenMsgCycleTime，没有该属性则只发送一次。0 不一定表示单次发送。")},
             {tr("没有信号定义时"),
              tr("原始 ID 按 HEX 模式组字节，此时模板和数据类型决定整帧内容；不具有按信号保留其他位的含义。")}}};
    case FieldHelpTopic::PeriodicSend:
        return {tr("周期发送"), tr("为按钮或数据发送卡片填写间隔。运行时点击开始发送，可在原按钮或「周期任务」里停止。"),
            {{tr("每秒发送一次"), QStringLiteral("1000")}},
            {{tr("发送内容"), tr("每轮读取输入框、发送卡片或工程数据点的当前值并重新组帧。输入无效时跳过该轮，可在「周期任务」查看原因。")},
             {tr("下发前确认"), tr("确认一次即可启动。整个周期任务使用启动时的发送目标和间隔。")},
             {tr("何时停止"), tr("目标断开、改变或运行结束时停止。重新连接后可手动开始。")}}};
    case FieldHelpTopic::CanSignalDefinitions:
        return {
            tr("DBC 与 CAN 信号"),
            tr("在 CAN 配置卡打开「可视化编辑…」，按报文 ID 查看 DBC 信号或补充内联信号。选中信号会高亮对应位，粘贴样例报文即可试解析。"),
            {{tr("转速信号：十六进制 ID 102，第 16 位起，16 位无符号"),
              QStringLiteral("102 EngSpeed : 16|16@1+ (1,0) [0|8000] \"rpm\"")},
             {tr("单个位：ID 102 的第 0 位"), QStringLiteral("102 Enable : 0|1@1+ (1,0) [0|1] \"\"")}},
            {{tr("编辑文本…"),
              tr("格式为 ID 信号名 : 起始位|位长@字节序符号 (因子,偏移) [最小值|最大值] \"单位\"。位号从 0 开始；@1 是 Intel，@0 是 Motorola 的 DBC MSB 编号；+ 为无符号，- 为有符号。物理值 = 原始值 × 因子 + 偏移。")},
             {tr("报文名、信号名和注释"),
              tr("监视器「名称」显示匹配 ID 的 DBC 报文名，如 IO、HMI、TEST。绑定字段使用 报文名.信号名，如 IO.B4b4；发送时选择 HMI，再在模板中写 B4b4={value}。DBC 注释中的 LED_Red 等描述不等于实际信号名。")},
             {tr("名称不显示时"),
              tr("检查当前 CAN 卡片是否加载了正确 DBC，以及实际报文 ID 和标准/扩展帧格式是否匹配。信号名称和报文名称是两层信息；显示电压、电流并不意味着报文名称应为 IO。")},
             {tr("DBC 文件路径"), tr("相对路径以工程文件所在目录为基准。保存工程后将 DBC 与工程一起保留；导出应用时会复制已引用的 DBC 文件。")}}};
    case FieldHelpTopic::CanTiming:
        return {tr("采样点与自定义位时序"),
                tr("ZLG USBCAN-I/II 系列的采样点默认使用设备标准。也可选择具体百分比或填写自定义参数。「时序」显示实际值，点击「计算」可选择并回填参数。"),
                {{tr("USBCAN-I/II：500k、87.5%、SJW=1 的 BTR 参数"), QStringLiteral("00,1C")},
                 {tr("同一组合：十进制 BRP,TSEG1,TSEG2,SJW"), QStringLiteral("1,13,2,1")},
                 {tr("同一型号的 ZCANPRO 完整结果也可粘贴"), QStringLiteral("500Kbps(87%),(00,1C)")}},
                {{tr("采样点是什么"), tr("采样点是每个 CAN 位内读取总线电平的位置。可用位置受硬件时间量子限制，实际值可能与目标值略有差别；请以卡片显示的实际时序为准。")},
                 {tr("自定义与下拉框"), tr("填写自定义后，以自定义参数为准，波特率和采样点下拉框停用；清空后恢复自动或设备标准设置。修改后点击连接才会配置硬件，连接期间参数锁定。")},
                 {tr("其他 ZLG 型号"),
                  tr("E-U 和 CAN FD 型号应从 ZCANPRO 中选择实际型号，再粘贴完整计算结果；不要套用 USBCAN-I/II 的 00,1C。USBCAN-4E-U 使用十进制 bps，其接口不提供独立采样点设置。")},
                 {tr("保存计算结果"), tr("计算器的「应用到卡片」只回填当前运行卡片。要作为工程默认值，请复制到设计器的「ZLG 自定义位时序」属性并保存工程。")},
                 {tr("设备与通道"), tr("设备编号和物理通道都从 0 开始。设备编号用于区分同型号的多块适配器。")}}};
    case FieldHelpTopic::CanReceiveFilter:
        return {
            tr("CAN 接收过滤"),
            tr("留空接收全部报文；填写后只接收列出的 ID 或范围。ID 使用十六进制，可用逗号、空格或换行分隔。"),
            {{tr("接收一段标准 ID 和一个扩展 ID"), QStringLiteral("100-1FF, 18FF0A01")},
             {tr("接收数值相同的标准帧和扩展帧"), QStringLiteral("123, 00000123")}},
            {{tr("标准帧与扩展帧"),
              tr("不大于 7FF 的短 ID 默认标准帧；八位十六进制写法明确指定扩展帧。数值相同的标准帧和扩展帧分别匹配。")},
             {tr("与监视器筛选的区别"), tr("连接卡片的接收过滤影响整条链路的绑定和反馈缓存，修改后需要重连。监视器上方的筛选只改变表格和 CSV 显示，不会阻止其他控件收到报文。")},
             {tr("使用反馈映射时"), tr("必须允许接收映射右侧的反馈报文，否则连接时会报告配置冲突。过滤条件使用 ID，不能填写 IO 或 HMI 等报文名。")}}};
    case FieldHelpTopic::DataRouting:
        return {tr("连接与子通道"), tr("来源连接决定控件读取哪张连接卡的数据，发送连接决定命令发到哪张连接卡。两者可以分别设置。"),
            {{tr("两路 CAN 使用相同 ID"), tr("把两个显示控件的来源连接分别选为 CAN_A 和 CAN_B；控制按钮的发送连接也选相应的卡片。CAN ID 不承担通道选择。")},
             {tr("TCP、UDP 和 MQTT"), tr("子通道填写 TCP 服务端客户端或 UDP 发送方的 IP:端口，例如 192.168.1.20:502；IPv6 使用 [地址]:端口。MQTT 填具体 Topic，例如 device/1/state，按完整名称匹配。")}},
            {{tr("自动匹配与广播"), tr("绑定和发送只有一个匹配连接时可以自动选用。有多个连接时请选择来源或目标。发送到多条连接需要显式选择广播。单个 TCP 服务端有多个在线客户端时，请填写发送子通道。")},
             {tr("切换连接"), tr("子通道随所选连接显示。更换连接后请按新连接重新填写子通道。")},
             {tr("独立缓存"), tr("各连接以及各客户端、UDP 发送方、MQTT Topic 独立拼帧。断开或重连一个连接不会清空其他连接的接收缓存。S7 必须选择具体 S7 卡片。")}}};
    case FieldHelpTopic::CanMonitorModes:
        return {
            tr("报文监视模式"),
            tr("「滚动日志」适合查看报文先后顺序；「按 ID 汇总」适合观察各 ID 的当前数据和统计。两种模式都支持筛选、条数上限和 CSV 导出。"),
            {},
            {{tr("滚动日志"), tr("每帧一行，按到达顺序显示；隐藏周期和次数，禁止表头排序。「自动滚动」控制是否跟随最新行，关闭后可翻阅已有记录。")},
             {tr("按 ID 汇总"),
              tr("按连接、ID、标准/扩展格式和收发方向合并，显示最新数据、周期和次数，允许排序；隐藏自动滚动。周期按最近最多 10 次报文间隔计算平均值。")},
             {tr("序号与切换模式"),
              tr("序号随进入监视器的收发报文累计；筛选和淘汰记录不重新编号。汇总模式显示该项最近一次更新的序号。切到滚动时只带入各汇总项的最新一帧；切回汇总时合并当前保留的记录。")},
             {tr("显示与保存行数"), tr("「显示行数」只限制表格保留记录；「每文件行数」决定自动保存的 CSV 分段大小，两者独立。新控件默认每文件 100000 行；0.8 之前版本保存的工程未设置此项时沿用原显示上限。修改每文件行数会写完当前尾段，再按新值记录；修改显示行数不影响保存。")},
             {tr("自动保存"), tr("勾选后先选择目录，后台保存所选来源连接的全部收发报文。每个 CSV 达到「每文件行数」就新建下一份，关闭保存、断开最后一个所选 CAN 连接或退出运行时，会保存不足一份的尾段。显示模式、ID 筛选、清空表格均不影响记录；再次连接会开始新一组文件。目录可在属性中预设，已有文件不会覆盖或自动删除。写盘失败会停止保存并提示，通信继续。")},
             {tr("保存目录"), tr("自动保存开启时，文件夹按钮可以打开或更换目录。更换目录后会写完旧目录的尾段，后续报文进入新目录。")},
             {tr("估算负载"), tr("按连接卡实际采集的收发帧和位填充上限估算。能确定通道速率时显示估算值。表格筛选只影响可见报文。")},
             {tr("导出内容"),
              tr("CSV 导出当前筛选后的保留记录，滚动模式省略周期与次数。time 包含日期、毫秒和时区；timestamp_ms 保留原始整数时间。已清空或超出上限淘汰的记录不在导出范围内。")}}};
    }
    return {};
}

// clang-format on

QDialog *showHelp(const HelpContent &content, QWidget *parent)
{
    auto *dialog = new AppDialog(content.title, parent);
    dialog->setObjectName(QStringLiteral("fieldHelpDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("帮助：%1").arg(content.title));
    auto *layout = dialog->viewLayout();
    auto *scroll = new FluentQt::ScrollArea(dialog);
    scroll->setObjectName(QStringLiteral("fieldHelpScroll"));
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->enableTransparentBackground();
    auto *body = new QWidget(scroll);
    auto *items = new QVBoxLayout(body);
    items->setContentsMargins(0, 0, 8, 0);
    items->setSpacing(12);
    const auto paragraph = [](const QString &text, QWidget *parent) {
        auto *label = new FluentQt::BodyLabel(text, parent);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        label->setMinimumWidth(0);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        return label;
    };
    items->addWidget(paragraph(content.introduction, body));
    for (int i = 0; i < content.examples.size(); ++i) {
        const HelpText &example = content.examples.at(i);
        auto *card = new FluentQt::SimpleCardWidget(body);
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(12, 10, 12, 12);
        auto *header = new QHBoxLayout;
        header->addWidget(paragraph(example.title, card), 1);
        auto *copy = new FluentQt::PushButton(tr("复制示例"), card);
        copy->setAutoDefault(false);
        copy->setObjectName(QStringLiteral("fieldHelpCopy%1").arg(i));
        copy->setAccessibleName(tr("复制示例：%1").arg(example.title));
        header->addWidget(copy);
        cardLayout->addLayout(header);
        auto *code = new FluentQt::PlainTextEdit(card);
        code->setObjectName(QStringLiteral("fieldHelpExample%1").arg(i));
        code->setAccessibleName(example.title);
        code->setReadOnly(true);
        code->setLineWrapMode(QPlainTextEdit::NoWrap);
        code->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        code->setPlainText(example.body);
        code->setFixedHeight((example.body.count(QLatin1Char('\n')) + 1) * code->fontMetrics().lineSpacing() + 42);
        cardLayout->addWidget(code);
        QObject::connect(copy, &QPushButton::clicked, dialog, [copy, text = example.body]() {
            QGuiApplication::clipboard()->setText(text);
            copy->setText(tr("已复制"));
            QTimer::singleShot(1500, copy, [copy]() { copy->setText(tr("复制示例")); });
        });
        items->addWidget(card);
    }
    for (const HelpText &detail : content.details) {
        auto *title = new FluentQt::StrongBodyLabel(detail.title, body);
        title->setWordWrap(true);
        items->addWidget(title);
        items->addWidget(paragraph(detail.body, body));
    }
    items->addStretch();
    scroll->setWidget(body);
    layout->addWidget(scroll, 1);
    if (!content.examples.isEmpty()) {
        layout->addWidget(paragraph(tr("点击“复制示例”，再粘贴到对应字段。"), dialog));
    }
    dialog->hideCancelButton();
    auto *close = dialog->yesButton();
    close->setText(tr("关闭"));
    close->setObjectName(QStringLiteral("fieldHelpClose"));
    close->setDefault(true);
    dialog->setPreferredSize(QSize(680, 700));
    dialog->open();
    // Short help topics fit their text; longer topics retain the scrollable height cap.
    dialog->widget()->layout()->activate();
    const int chrome = dialog->widget()->height() - scroll->height() + 2 * scroll->frameWidth();
    const int bodyHeight = items->totalHeightForWidth(scroll->viewport()->width());
    dialog->setPreferredSize(QSize(680, qMin(700, chrome + bodyHeight)));
    return dialog;
}

} // namespace

std::optional<FieldHelpTopic> fieldHelpTopic(QStringView key)
{
    static const struct {
        const char *key;
        FieldHelpTopic topic;
    } topics[] = {{"layout.pages", FieldHelpTopic::PageNavigation},
                  {"widget.appearance", FieldHelpTopic::WidgetAppearance},
                  {"widget.selection", FieldHelpTopic::SelectionStyles},
                  {"input.number", FieldHelpTopic::NumberInput},
                  {"alarm.filters", FieldHelpTopic::AlarmFilters},
                  {"data.routing", FieldHelpTopic::DataRouting},
                  {"can.initial-state", FieldHelpTopic::CanInitialState},
                  {"can.signal-command", FieldHelpTopic::CanSignalCommand},
                  {"can.signal-definitions", FieldHelpTopic::CanSignalDefinitions},
                  {"can.timing", FieldHelpTopic::CanTiming},
                  {"can.receive-filter", FieldHelpTopic::CanReceiveFilter},
                  {"can.monitor-modes", FieldHelpTopic::CanMonitorModes},
                  {"data.binding", FieldHelpTopic::DataBinding},
                  {"data.expression", FieldHelpTopic::TransformExpression},
                  {"display.enum", FieldHelpTopic::EnumDisplay},
                  {"display.threshold", FieldHelpTopic::ValueThreshold},
                  {"project.logic", FieldHelpTopic::ProjectLogic},
                  {"can.recovery", FieldHelpTopic::CanRecovery},
                  {"command.templates", FieldHelpTopic::CommandTemplates},
                  {"command.periodic", FieldHelpTopic::PeriodicSend},
                  {"modbus.addressing", FieldHelpTopic::ModbusAddressing},
                  {"s7.addressing", FieldHelpTopic::S7Addressing},
                  {"receive.framing", FieldHelpTopic::ReceiveFraming},
                  {"industrial.text-layout", FieldHelpTopic::IndustrialTextLayout}};
    for (const auto &entry : topics) {
        if (key == QLatin1String(entry.key)) {
            return entry.topic;
        }
    }
    return std::nullopt;
}

QToolButton *createFieldHelpButton(FieldHelpTopic topic, QWidget *parent)
{
    auto *button = new FluentQt::TransparentToolButton(FluentQt::icon(FluentQt::FluentIcon::Info), parent);
    button->setIconSize(QSize(16, 16));
    const QString style = QStringLiteral("QToolButton { padding: 0px; min-width: 0px; min-height: 0px; }");
    FluentQt::FluentStyleSheet::setCustomStyleSheet(button, style, style);
    button->setFixedSize(24, 24);
    button->setAccessibleName(tr("查看%1的帮助").arg(contentFor(topic).title));
    // The window owns the help, so rebuilding the property panel cannot leave a dangling callback.
    QObject::connect(button, &QToolButton::clicked, button,
                     [button, topic]() { showHelp(contentFor(topic), visibleWindow(button)); });
    return button;
}

} // namespace fhmi
