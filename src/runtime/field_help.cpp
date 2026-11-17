#include "runtime/field_help.h"

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
#include <QtGui/QScreen>
#include <QtWidgets/QDialog>
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
            tr("数据绑定决定控件从收到的数据中读取什么；发送属性决定用户操作时写入什么。两者分别设置，接收更新不会自动发送命令。曲线的解析模式只作用于当前编辑的那条曲线。"),
            {{tr("键值匹配：收到 temperature=25.3 时，绑定字段填"), QStringLiteral("temperature")},
             {tr("JSON 路径：从 data.sensors 数组第一项读取 temp"), QStringLiteral("data.sensors.0.temp")}},
            {{tr("如何选择解析模式"),
              tr("文本键值、已解析的 HEX 字段或 DBC 信号使用「键值匹配」；嵌套 JSON 使用「JSON路径」，数组下标从 0 开始。DBC 信号填写 报文名.信号名，例如 IO.B4b4。没有 DBC 时可用「CAN信号」手工指定 ID 和位位置。")},
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
              tr("可用 + - * / // % **、整数位运算 & | ^ ~ << >>、比较、and / or / not 和括号。比较产生 True 或 False；曲线将其记录为 1 或 0。表达式不能调用函数，也不能引用其他控件或多个寄存器。")},
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
             {tr("Modbus、CAN 和 S7"),
              tr("Modbus 使用功能码、地址和值；S7 使用目标连接、绝对地址和写入值；它们不使用文本模板。CAN 有信号定义时使用 信号={value}，可点击「发送模板」旁的帮助查看保留其他位的规则。")},
             {tr("何时发送"),
              tr("按钮点击、开关拨动、滑块松手或输入框回车时触发发送。文本、HEX、JSON 和 CAN 模式需开启相应的「改变时发送」「回车发送」等开关；Modbus 和 S7 模式直接按操作发送。设备反馈只更新控件，不会回发相同命令。")}}};
    case FieldHelpTopic::ModbusAddressing:
        return {
            tr("Modbus 地址与采集"),
            tr("先选择寄存器区，再填写从 0 开始的协议地址。设备手册中的 40001 若表示第一个保持寄存器，界面应选「保持寄存器」、地址填 0；请按手册确认基准，不要直接把 40001 当偏移。"),
            {{tr("模拟从站：填入连接卡片的「初始值」，地址须在已配置表区内"), QStringLiteral("hr10=1234\nco3=1")}},
            {{tr("读取、写入分别配置"),
              tr("数据绑定中的地址用于读取，发送属性中的地址用于写入。例如开关读写保持寄存器 10：两处都选保持寄存器、地址 10、同一从站；开关发送开=1、关=0。输入寄存器和离散输入是读取区。")},
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
              tr("「开启判定」控制收到什么值时显示为开；S7「开启写入值」「关闭写入值」控制用户拨动时写什么。BOOL 写入只修改该位，保留相邻位。收到反馈不会触发写入。")}}};
    case FieldHelpTopic::ReceiveFraming:
        return {
            tr("接收分帧与字段"),
            tr("连接卡片先把收到的字节整理成完整记录，再交给控件取值。「项目设置」使用工程的解析模式与帧格式；卡片选择自己的模式时可单独配置。"),
            {{tr("line：行结束符填写反斜杠 n"), QStringLiteral("\\n")},
             {tr("hex：字段表每行一个，省略字节序时使用大端"), QStringLiteral("temperature,0,2,uint16")}},
            {{tr("行文本何时更新"),
              tr("line 模式默认以换行结束一条文本记录，逗号分项、等号分隔键和值。设备没有发出行结束符时会继续等候。完整 JSON 对象或数组按括号边界解析，无需换行；MQTT 消息、UDP 数据报有自己的边界。")},
             {tr("二进制帧如何结束"),
              tr("hex 模式至少配置帧尾、长度字段、固定帧长、空闲超时中的一种。固定帧长包含帧头和帧尾；长度字段偏移从整帧第 0 字节算起。空闲超时表示等到连续一段时间没有新字节才交付帧，并非固定刷新周期。")},
             {tr("字段偏移从哪里算"),
              tr("字段表格式为 名字,偏移,长度,类型[,字节序]，偏移从帧头之后的第 0 字节算起。帧头 AA55、帧尾 55AA、数据 AA55 00FD 55AA，配合上面的字段表得到 temperature=253；控件用「键值匹配」绑定 temperature。")},
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
    case FieldHelpTopic::CanSignalDefinitions:
        return {
            tr("DBC 与 CAN 信号"),
            tr("DBC 定义报文 ID、报文名和各信号的位布局。「CAN 信号」可补充 DBC 之外的字段，每行一个定义；已有 DBC 字段无需重复填写。"),
            {{tr("转速信号：十六进制 ID 102，第 16 位起，16 位无符号"),
              QStringLiteral("102 EngSpeed : 16|16@1+ (1,0) [0|8000] \"rpm\"")},
             {tr("单个位：ID 102 的第 0 位"), QStringLiteral("102 Enable : 0|1@1+ (1,0) [0|1] \"\"")}},
            {{tr("格式含义"),
              tr("格式为 ID 信号名 : 起始位|位长@字节序符号 (因子,偏移) [最小值|最大值] \"单位\"。位号从 0 开始；@1 是 Intel，@0 是 Motorola 的 DBC MSB 编号；+ 为无符号，- 为有符号。物理值 = 原始值 × 因子 + 偏移。")},
             {tr("报文名、信号名和注释"),
              tr("监视器「名称」显示匹配 ID 的 DBC 报文名，如 IO、HMI、TEST。绑定字段使用 报文名.信号名，如 IO.B4b4；发送时选择 HMI，再在模板中写 B4b4={value}。DBC 注释中的 LED_Red 等描述不等于实际信号名。")},
             {tr("名称不显示时"),
              tr("检查当前 CAN 卡片是否加载了正确 DBC，以及实际报文 ID 和标准/扩展帧格式是否匹配。信号名称和报文名称是两层信息；显示电压、电流并不意味着报文名称应为 IO。")},
             {tr("DBC 文件路径"), tr("相对路径以工程文件所在目录为基准。保存工程后将 DBC 与工程一起保留；导出应用时会复制已引用的 DBC 文件。")}}};
    case FieldHelpTopic::CanTiming:
        return {tr("采样点与自定义位时序"),
                tr("ZLG USBCAN-I/II 系列可直接选择波特率和采样点，自定义留空即可自动计算。「时序」显示硬件实际可实现的值，也可点击「计算」选择并回填参数。"),
                {{tr("USBCAN-I/II：500k、87.5%、SJW=1 的 BTR 参数"), QStringLiteral("00,1C")},
                 {tr("同一组合：十进制 BRP,TSEG1,TSEG2,SJW"), QStringLiteral("1,13,2,1")},
                 {tr("同一型号的 ZCANPRO 完整结果也可粘贴"), QStringLiteral("500Kbps(87%),(00,1C)")}},
                {{tr("采样点是什么"), tr("采样点是每个 CAN 位内读取总线电平的位置。可用位置受硬件时间量子限制，实际值可能与目标值略有差别；请以卡片显示的实际时序为准。")},
                 {tr("自定义与下拉框"), tr("填写自定义后，以自定义参数为准，波特率和采样点下拉框停用；清空后恢复自动或设备标准设置。修改后点击连接才会配置硬件，连接期间参数锁定。")},
                 {tr("其他 ZLG 型号"),
                  tr("E-U 和 CAN FD 型号应从 ZCANPRO 中选择实际型号，再粘贴完整计算结果；不要套用 USBCAN-I/II 的 00,1C。USBCAN-4E-U 使用十进制 bps，其接口不提供独立采样点设置。")},
                 {tr("保存计算结果"), tr("计算器的「应用到卡片」只回填当前运行卡片。要作为工程默认值，请复制到设计器的「自定义位时序」属性并保存工程。")}}};
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
    case FieldHelpTopic::CanMonitorModes:
        return {
            tr("报文监视模式"),
            tr("「滚动日志」适合查看报文先后顺序；「按 ID 汇总」适合观察各 ID 的当前数据和统计。两种模式都支持筛选、条数上限和 CSV 导出。"),
            {},
            {{tr("滚动日志"), tr("每帧一行，按到达顺序显示；隐藏周期和次数，禁止表头排序。「自动滚动」控制是否跟随最新行，关闭后可翻阅已有记录。")},
             {tr("按 ID 汇总"),
              tr("按 ID、标准/扩展格式和收发方向合并，显示最新数据、周期和次数，允许排序；隐藏自动滚动。周期按最近最多 10 次报文间隔计算平均值。")},
             {tr("序号与切换模式"),
              tr("序号随进入监视器的收发报文累计；筛选和淘汰记录不重新编号。汇总模式显示该项最近一次更新的序号。切到滚动时只带入各汇总项的最新一帧；切回汇总时合并当前保留的记录。")},
             {tr("导出内容"),
              tr("CSV 导出当前筛选后的保留记录，滚动模式省略周期与次数。time 包含日期、毫秒和时区；timestamp_ms 保留原始整数时间。已清空或超出上限淘汰的记录不在导出范围内。")}}};
    }
    return {};
}

// clang-format on

QDialog *showHelp(const HelpContent &content, QWidget *parent)
{
    auto *dialog = new QDialog(parent);
    dialog->setObjectName(QStringLiteral("fieldHelpDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("帮助：%1").arg(content.title));
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(12);
    auto *heading = new FluentQt::SubtitleLabel(content.title, dialog);
    heading->setWordWrap(true);
    layout->addWidget(heading);
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
    auto *footer = new QHBoxLayout;
    if (!content.examples.isEmpty()) {
        footer->addWidget(paragraph(tr("点击“复制示例”，再粘贴到对应字段。"), dialog), 1);
    } else {
        footer->addStretch();
    }
    auto *close = new FluentQt::PrimaryPushButton(tr("关闭"), dialog);
    close->setObjectName(QStringLiteral("fieldHelpClose"));
    close->setDefault(true);
    footer->addWidget(close);
    layout->addLayout(footer);
    QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
    dialog->setMinimumSize(320, 260);
    const QSize available = parent->screen()->availableGeometry().size() - QSize(48, 48);
    dialog->resize(QSize(680, 700).boundedTo(available));
    dialog->open();
    return dialog;
}

} // namespace

QToolButton *createFieldHelpButton(FieldHelpTopic topic, QWidget *parent)
{
    auto *button = new FluentQt::TransparentToolButton(FluentQt::icon(FluentQt::FluentIcon::Question), parent);
    button->setIconSize(QSize(16, 16));
    const QString style = QStringLiteral("QToolButton { padding: 0px; min-width: 0px; min-height: 0px; }");
    FluentQt::FluentStyleSheet::setCustomStyleSheet(button, style, style);
    button->setFixedSize(24, 24);
    button->setAccessibleName(tr("查看%1的帮助").arg(contentFor(topic).title));
    fluentToolTip(button, button->accessibleName());
    // A separate window owns the help, so rebuilding the property panel cannot leave a dangling callback.
    QObject::connect(button, &QToolButton::clicked, button,
                     [button, topic]() { showHelp(contentFor(topic), button->window()); });
    return button;
}

} // namespace fhmi
