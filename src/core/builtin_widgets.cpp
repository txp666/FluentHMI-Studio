// Metadata of every designable widget. Type names, property names, defaults and enum
// options match PyQt UI Designer exactly, so .uidp projects open in either program.
#include "core/widget_meta.h"

#include "core/binding.h"
#include "core/command_encoder.h"

namespace fhmi {

namespace {

PropDef str(const QString &name, const QString &label, const QString &def, bool bindable = false)
{
    PropDef p{name, label, PropType::String, def, {}, std::nullopt, std::nullopt, bindable, {}};
    return p;
}

PropDef text(const QString &name, const QString &label, const QString &def)
{
    return PropDef{name, label, PropType::Text, def, {}, std::nullopt, std::nullopt, false, {}};
}

PropDef command(const QString &name, const QString &label)
{
    return PropDef{name, label, PropType::Command, QString(), {}, std::nullopt, std::nullopt, false, {}};
}

PropDef integer(const QString &name, const QString &label, int def, double lo, double hi, bool bindable = false)
{
    return PropDef{name, label, PropType::Int, def, {}, lo, hi, bindable, {}};
}

PropDef real(const QString &name, const QString &label, double def, double lo, double hi, bool bindable = false)
{
    return PropDef{name, label, PropType::Float, def, {}, lo, hi, bindable, {}};
}

PropDef boolean(const QString &name, const QString &label, bool def, bool bindable = false)
{
    return PropDef{name, label, PropType::Bool, def, {}, std::nullopt, std::nullopt, bindable, {}};
}

PropDef choice(const QString &name, const QString &label, const QString &def, const QStringList &options)
{
    return PropDef{name, label, PropType::Enum, def, options, std::nullopt, std::nullopt, false, {}};
}

PropDef described(PropDef def, const QString &description)
{
    def.description = description;
    return def;
}

// A link card's own frame format: the keys of the project's serial.parserConfig.
PropDef parserConfig()
{
    return PropDef{
        QStringLiteral("parserConfig"),
        QT_TR_NOOP("帧格式"),
        PropType::Object,
        QJsonObject(),
        {},
        std::nullopt,
        std::nullopt,
        false,
        // Descriptions are for the widget reference (docs/uidp-widgets.md) only, which is Chinese.
        QStringLiteral(
            "帧格式设置，键同工程的 serial.parserConfig；默认解析模式不是「项目设置」时生效，" // i18n: ignore
            "空对象表示沿用工程设置（模式相同时）或默认值")};                                  // i18n: ignore
}

const QStringList kCommandModes = {QStringLiteral("文本"), // i18n: value
                                   QStringLiteral("HEX"), QStringLiteral("JSON"), QStringLiteral("Modbus"),
                                   QStringLiteral("CAN")};
const QStringList kHexTypes = {
    QStringLiteral("HEX字符串"), QStringLiteral("uint8"),  QStringLiteral("int8"), // i18n: value
    QStringLiteral("uint16"),    QStringLiteral("int16"),  QStringLiteral("uint32"),
    QStringLiteral("int32"),     QStringLiteral("float32")};
const QStringList kNumericTypes = {QStringLiteral("uint8"),  QStringLiteral("int8"),  QStringLiteral("uint16"),
                                   QStringLiteral("int16"),  QStringLiteral("uint32"), QStringLiteral("int32"),
                                   QStringLiteral("float32")};
const QStringList kByteOrders = {QStringLiteral("大端(BE)"), QStringLiteral("小端(LE)")};             // i18n: value
const QStringList kNewlines = {QStringLiteral("无"), QStringLiteral("\\r\\n"), QStringLiteral("\\n"), // i18n: value
                               QStringLiteral("\\r")};
const QStringList kAllFunctions = CommandEncoder::functionOptions();
const QStringList kInputFunctions = CommandEncoder::functionOptions({0x03, 0x04, 0x05, 0x06, 0x10});
const QStringList kWriteFunctions = CommandEncoder::functionOptions({0x05, 0x06, 0x10});
const QStringList kNotifyTypes = {QStringLiteral("成功"), QStringLiteral("信息"), QStringLiteral("警告"), // i18n: value
                                  QStringLiteral("错误")};                                                // i18n: value
const QStringList kDisplayFormats = {QStringLiteral("自动"), QStringLiteral("十进制"),                    // i18n: value
                                     QStringLiteral("十六进制(HEX)"), QStringLiteral("二进制(BIN)")};     // i18n: value
const QStringList kDecimals = {QStringLiteral("自动"), QStringLiteral("0"), QStringLiteral("1"),          // i18n: value
                               QStringLiteral("2"),    QStringLiteral("3"), QStringLiteral("4")};
const QStringList kLinkParsers = {
    QStringLiteral("项目设置"), QStringLiteral("line"),       QStringLiteral("hex"), // i18n: value
    QStringLiteral("modbus"),   QStringLiteral("modbus_tcp"), QStringLiteral("modbus_ascii")};

// A link card's parser: its mode; what it hands the Modbus poller while its link is connected and parses
// Modbus (the property editor shows these only for a Modbus mode of the card's own); its frame format. A serial,
// TCP or UDP card can also be a Modbus slave (`slave`): its role, how it answers and its data table.
QList<PropDef> linkParsing(bool slave)
{
    QList<PropDef> props{choice(QStringLiteral("defaultParser"), QT_TR_NOOP("默认解析模式"),
                                QStringLiteral("项目设置"), // i18n: value
                                kLinkParsers)};
    if (slave) {
        props.append(described(choice(QStringLiteral("modbusRole"), QT_TR_NOOP("Modbus 角色"),
                                      QStringLiteral("主站"),                                            // i18n: value
                                      {QStringLiteral("主站"), QStringLiteral("从站")}),                 // i18n: value
                               QStringLiteral("从站：解析模式为 modbus 或 modbus_tcp 时应答外部主站"))); // i18n: ignore
    }
    props += {integer(QStringLiteral("modbusPeriod"), QT_TR_NOOP("默认轮询(ms)"), 500, 10, 60000),
              integer(QStringLiteral("modbusTimeout"), QT_TR_NOOP("响应超时(ms)"), 1000, 10, 60000),
              integer(QStringLiteral("modbusSlave"), QT_TR_NOOP("默认从站"), 1, 1, 247)};
    if (slave) {
        props += {integer(QStringLiteral("modbusDelay"), QT_TR_NOOP("响应延时(ms)"), 0, 0, 5000),
                  described(boolean(QStringLiteral("modbusExceptions"), QT_TR_NOOP("异常响应"), true),
                            QStringLiteral("关掉后出错的请求不应答"))}; // i18n: ignore
        struct Table
        {
            const char *name;
            const char *start;
            const char *count;
        };
        static constexpr Table tables[] = {{"Co", QT_TR_NOOP("线圈起始"), QT_TR_NOOP("线圈数量")},
                                           {"Di", QT_TR_NOOP("离散输入起始"), QT_TR_NOOP("离散输入数量")},
                                           {"Hr", QT_TR_NOOP("保持寄存器起始"), QT_TR_NOOP("保持寄存器数量")},
                                           {"Ir", QT_TR_NOOP("输入寄存器起始"), QT_TR_NOOP("输入寄存器数量")}};
        for (const Table &table : tables) {
            const QString name = QLatin1String(table.name);
            props += {integer(QStringLiteral("slave%1Start").arg(name), QString::fromUtf8(table.start), 0, 0, 65535),
                      integer(QStringLiteral("slave%1Count").arg(name), QString::fromUtf8(table.count), 100, 0, 10000)};
        }
        props.append(described(text(QStringLiteral("slaveInit"), QT_TR_NOOP("初始值"), QString()),
                               QStringLiteral("每行一条 `区地址=值`，如 `hr100=1234`、`co3=1`；" // i18n: ignore
                                              "寄存器 −32768–65535，线圈、离散输入 0 或 1")));   // i18n: ignore
    }
    props.append(parserConfig());
    return props;
}

// The framing fields every command-sending widget shares, between the mode and Modbus.
QList<PropDef> hexFraming(const QStringList &dataTypes, const QString &dataTypeDefault, bool newlineFirst)
{
    QList<PropDef> out{str(QStringLiteral("frameHeader"), QT_TR_NOOP("帧头(HEX)"), QString()),
                       str(QStringLiteral("frameTail"), QT_TR_NOOP("帧尾(HEX)"), QString())};
    const PropDef newline =
        choice(QStringLiteral("commandNewline"), QT_TR_NOOP("换行符"), QStringLiteral("\\r\\n"), kNewlines);
    if (newlineFirst) {
        out.append(newline);
    }
    out.append(choice(QStringLiteral("dataType"), QT_TR_NOOP("数据类型"), dataTypeDefault, dataTypes));
    out.append(choice(QStringLiteral("byteOrder"), QT_TR_NOOP("字节序"),
                      QStringLiteral("大端(BE)"), // i18n: value
                      kByteOrders));
    if (!newlineFirst) {
        out.append(newline);
    }
    return out;
}

// CAN mode: the message a command sets (a DBC message's name, or an ID) and how it goes out. Only FluentHMI Studio
// sends CAN.
QList<PropDef> canCommand()
{
    return {described(str(QStringLiteral("canId"), QT_TR_NOOP("CAN 报文"), QString()),
                      QStringLiteral("DBC 报文名，或十六进制 ID（如 `102`、`18FF50E5`）；有信号的报文发送模板写 " // i18n: ignore
                                     "`信号=值`（十进制物理值，`0x` 开头为原始值），原始 ID 按 HEX 模式组字节")),                                // i18n: ignore
            described(boolean(QStringLiteral("canFd"), QT_TR_NOOP("CAN FD 帧"), false),
                      QStringLiteral("只用于原始 ID；DBC 报文是否 FD 由 DBC 决定")), // i18n: ignore
            described(integer(QStringLiteral("canCycle"), QT_TR_NOOP("循环周期(ms)"), 0, 0, 60000),
                      QStringLiteral("大于 0 时连接期间按此周期重发；0 时用 DBC 的 GenMsgCycleTime，没有则只发一次"))}; // i18n: ignore
}

QList<PropDef> notification(const QString &event, const QString &content, const QString &type)
{
    return {boolean(QStringLiteral("notify"), event, false),
            str(QStringLiteral("notifyTitle"), QT_TR_NOOP("通知标题"), QT_TR_NOOP("提示")),
            str(QStringLiteral("notifyContent"), QT_TR_NOOP("通知内容"), content),
            choice(QStringLiteral("notifyType"), QT_TR_NOOP("通知类型"), type, kNotifyTypes),
            integer(QStringLiteral("notifyDuration"), QT_TR_NOOP("通知时长(ms)"), 3000, 500, 30000)};
}

WidgetMeta meta(const QString &type, const QString &display, WidgetCategory category, const QString &icon, int w,
                int h, QList<PropDef> props, int minW = 30, int minH = 20, bool container = false)
{
    WidgetMeta m;
    m.typeName = type;
    m.displayName = display;
    m.category = category;
    m.icon = icon;
    m.defaultWidth = w;
    m.defaultHeight = h;
    m.properties = std::move(props);
    m.minWidth = minW;
    m.minHeight = minH;
    m.container = container;
    return m;
}

QList<WidgetMeta> basicWidgets()
{
    const PropDef mode =
        choice(QStringLiteral("commandMode"), QT_TR_NOOP("发送模式"), QStringLiteral("文本"), // i18n: value
               kCommandModes);
    const PropDef tpl = str(QStringLiteral("commandTemplate"), QT_TR_NOOP("发送模板"), QStringLiteral("{value}"));
    // JSON mode's own line ending, so that it defaults to none while 文本 keeps CR LF.
    const PropDef jsonNewline = choice(QStringLiteral("commandJsonNewline"), QT_TR_NOOP("换行符"),
                                       QStringLiteral("无"), // i18n: value
                                       kNewlines);
    const PropDef slave = integer(QStringLiteral("mbSlave"), QT_TR_NOOP("从站地址"), 1, 1, 247);
    const PropDef address = integer(QStringLiteral("mbAddress"), QT_TR_NOOP("寄存器地址"), 0, 0, 65535);
    // A count for reads, a whole number to write (negative for int16 / int32) otherwise.
    const PropDef mbValue = integer(QStringLiteral("mbValue"), QT_TR_NOOP("值/数量"), 1, -2147483648.0, 2147483647.0);
    // How a written value fills registers: FC 06 takes a 16-bit type, FC 16 any of them.
    const PropDef dataType = choice(QStringLiteral("mbDataType"), QT_TR_NOOP("数据类型"), QStringLiteral("uint16"),
                                    CommandEncoder::dataTypes());
    const PropDef regOrder = choice(QStringLiteral("mbRegOrder"), QT_TR_NOOP("寄存器序"),
                                    CommandEncoder::registerOrders().first(), CommandEncoder::registerOrders());

    QList<PropDef> button{str(QStringLiteral("text"), QT_TR_NOOP("文本"), QT_TR_NOOP("按钮")),
                          boolean(QStringLiteral("enabled"), QT_TR_NOOP("启用"), true), mode,
                          command(QStringLiteral("command"), QT_TR_NOOP("发送内容")), tpl, jsonNewline};
    button += hexFraming(kHexTypes, QStringLiteral("HEX字符串"), false); // i18n: value
    button += {slave,
               choice(QStringLiteral("mbFunc"), QT_TR_NOOP("功能码"), QStringLiteral("03 读保持寄存器"), // i18n: value
                      kAllFunctions),
               address,
               dataType,
               regOrder,
               mbValue};
    button += canCommand();
    button += notification(QT_TR_NOOP("按下通知"), QT_TR_NOOP("按钮已按下"), QStringLiteral("成功")); // i18n: value

    QList<PropDef> label{
        str(QStringLiteral("text"), QT_TR_NOOP("文本"), QT_TR_NOOP("标签"), true),
        integer(QStringLiteral("fontSize"), QT_TR_NOOP("字号"), 14, 8, 72),
        boolean(QStringLiteral("bold"), QT_TR_NOOP("加粗"), false),
        choice(QStringLiteral("alignment"), QT_TR_NOOP("对齐"), QStringLiteral("Left"),
               {QStringLiteral("Left"), QStringLiteral("Center"), QStringLiteral("Right")}),
        choice(QStringLiteral("displayFormat"), QT_TR_NOOP("显示格式"), QStringLiteral("自动"), // i18n: value
               kDisplayFormats),
        choice(QStringLiteral("decimals"), QT_TR_NOOP("小数位数"), QStringLiteral("自动"), // i18n: value
               kDecimals)};

    QList<PropDef> lineEdit{str(QStringLiteral("placeholderText"), QT_TR_NOOP("占位文本"), QT_TR_NOOP("请输入...")),
                            str(QStringLiteral("text"), QT_TR_NOOP("默认值"), QString()),
                            boolean(QStringLiteral("readOnly"), QT_TR_NOOP("只读"), false),
                            boolean(QStringLiteral("sendOnEnter"), QT_TR_NOOP("回车发送"), false),
                            mode,
                            tpl,
                            jsonNewline};
    lineEdit += hexFraming(kHexTypes, QStringLiteral("HEX字符串"), false); // i18n: value
    lineEdit += {slave,
                 choice(QStringLiteral("mbFunc"), QT_TR_NOOP("功能码"), QStringLiteral("06 写单寄存器"), // i18n: value
                        kInputFunctions),
                 address, dataType, regOrder};
    lineEdit += canCommand();

    QList<PropDef> combo{text(QStringLiteral("items"), QT_TR_NOOP("选项"), QT_TR_NOOP("选项1\n选项2\n选项3")),
                         integer(QStringLiteral("currentIndex"), QT_TR_NOOP("默认选项"), 0, 0, 99),
                         boolean(QStringLiteral("sendOnSelect"), QT_TR_NOOP("选中发送"), false),
                         text(QStringLiteral("commands"), QT_TR_NOOP("发送命令表"), QString()),
                         mode,
                         tpl,
                         jsonNewline};
    combo += hexFraming(kHexTypes, QStringLiteral("HEX字符串"), false); // i18n: value
    combo += {slave,
              choice(QStringLiteral("mbFunc"), QT_TR_NOOP("功能码"), QStringLiteral("06 写单寄存器"), // i18n: value
                     kInputFunctions),
              address,
              dataType,
              regOrder,
              mbValue};
    combo += canCommand();

    QList<PropDef> toggle{str(QStringLiteral("label"), QT_TR_NOOP("标签"), QT_TR_NOOP("开关")),
                          boolean(QStringLiteral("checked"), QT_TR_NOOP("默认开启"), false),
                          mode,
                          command(QStringLiteral("onCommand"), QT_TR_NOOP("开启命令")),
                          command(QStringLiteral("offCommand"), QT_TR_NOOP("关闭命令")),
                          tpl,
                          jsonNewline};
    toggle += hexFraming(kHexTypes, QStringLiteral("HEX字符串"), false); // i18n: value
    toggle += {slave, integer(QStringLiteral("mbAddress"), QT_TR_NOOP("线圈地址"), 0, 0, 65535)};
    toggle += canCommand();
    toggle += notification(QT_TR_NOOP("切换通知"), QT_TR_NOOP("开关已切换"), QStringLiteral("信息")); // i18n: value

    const auto ranged = [&](int value, bool suffix) {
        QList<PropDef> props{integer(QStringLiteral("minimum"), QT_TR_NOOP("最小值"), 0, -99999, 99999),
                             integer(QStringLiteral("maximum"), QT_TR_NOOP("最大值"), 100, -99999, 99999),
                             integer(QStringLiteral("value"), QT_TR_NOOP("默认值"), value, -99999, 99999)};
        if (suffix) {
            props.append(str(QStringLiteral("suffix"), QT_TR_NOOP("后缀"), QString()));
        }
        props += {boolean(QStringLiteral("sendOnChange"), QT_TR_NOOP("变化发送"), false), mode, tpl, jsonNewline};
        props += hexFraming(kNumericTypes, QStringLiteral("uint16"), true);
        props += {slave,
                  choice(QStringLiteral("mbFunc"), QT_TR_NOOP("功能码"), QStringLiteral("06 写单寄存器"), // i18n: value
                         kWriteFunctions),
                  address, dataType, regOrder};
        props += canCommand();
        return props;
    };

    return {meta(QStringLiteral("PushButton"), QT_TR_NOOP("按钮"), WidgetCategory::Basic, QStringLiteral("button"), 120,
                 36, button),
            meta(QStringLiteral("Label"), QT_TR_NOOP("标签"), WidgetCategory::Basic, QStringLiteral("font"), 120, 30,
                 label),
            meta(QStringLiteral("LineEdit"), QT_TR_NOOP("输入框"), WidgetCategory::Basic, QStringLiteral("text_field"),
                 200, 36, lineEdit),
            meta(QStringLiteral("ComboBox"), QT_TR_NOOP("下拉框"), WidgetCategory::Basic, QStringLiteral("dropdown"),
                 160, 36, combo),
            meta(QStringLiteral("ToggleSwitch"), QT_TR_NOOP("开关"), WidgetCategory::Basic, QStringLiteral("toggle"),
                 140, 30, toggle),
            meta(QStringLiteral("Slider"), QT_TR_NOOP("滑块"), WidgetCategory::Basic, QStringLiteral("slider"), 200, 30,
                 ranged(50, false)),
            meta(QStringLiteral("SpinBox"), QT_TR_NOOP("数值框"), WidgetCategory::Basic, QStringLiteral("spin_box"),
                 140, 36, ranged(0, true))};
}

QList<WidgetMeta> layoutWidgets()
{
    const PropDef spacing = integer(QStringLiteral("spacing"), QT_TR_NOOP("间距"), 8, 0, 100);
    return {
        meta(QStringLiteral("HBoxLayout"), QT_TR_NOOP("水平布局"), WidgetCategory::Layout, QStringLiteral("columns"),
             300, 80, {str(QStringLiteral("title"), QT_TR_NOOP("标题"), QT_TR_NOOP("水平布局")), spacing}, 60, 40,
             true),
        meta(QStringLiteral("VBoxLayout"), QT_TR_NOOP("垂直布局"), WidgetCategory::Layout, QStringLiteral("rows"), 200,
             200, {str(QStringLiteral("title"), QT_TR_NOOP("标题"), QT_TR_NOOP("垂直布局")), spacing}, 60, 40, true),
        meta(QStringLiteral("FlexLayout"), QT_TR_NOOP("弹性布局"), WidgetCategory::Layout, QStringLiteral("layout"),
             400, 200,
             {str(QStringLiteral("title"), QT_TR_NOOP("标题"), QT_TR_NOOP("弹性布局")),
              choice(QStringLiteral("direction"), QT_TR_NOOP("方向"), QStringLiteral("row"),
                     {QStringLiteral("row"), QStringLiteral("column")}),
              spacing},
             100, 60, true),
        meta(QStringLiteral("GroupBox"), QT_TR_NOOP("分组框"), WidgetCategory::Layout, QStringLiteral("group_box"), 280,
             180, {str(QStringLiteral("title"), QT_TR_NOOP("标题"), QT_TR_NOOP("分组"))}, 80, 60, true),
        meta(QStringLiteral("TabWidget"), QT_TR_NOOP("选项卡"), WidgetCategory::Layout, QStringLiteral("tabs"), 350,
             250,
             {text(QStringLiteral("tabNames"), QT_TR_NOOP("标签名称"), QT_TR_NOOP("标签1\n标签2\n标签3")),
              integer(QStringLiteral("currentTab"), QT_TR_NOOP("当前标签页"), 0, 0, 9999),
              choice(QStringLiteral("tabLayout"), QT_TR_NOOP("当前页布局"), QStringLiteral("自由"), // i18n: value
                     {QStringLiteral("自由"), QStringLiteral("垂直"), QStringLiteral("水平"),       // i18n: value
                      QStringLiteral("弹性(Flex)")})},                                              // i18n: value
             150, 80, true),
        meta(QStringLiteral("Separator"), QT_TR_NOOP("分割线"), WidgetCategory::Layout, QStringLiteral("remove"), 200,
             2,
             {choice(QStringLiteral("orientation"), QT_TR_NOOP("方向"), QStringLiteral("horizontal"),
                     {QStringLiteral("horizontal"), QStringLiteral("vertical")})},
             20, 2),
        meta(QStringLiteral("CardContainer"), QT_TR_NOOP("卡片容器"), WidgetCategory::Layout, QStringLiteral("card"),
             300, 200,
             {str(QStringLiteral("title"), QT_TR_NOOP("标题"), QString()),
              choice(QStringLiteral("layout"), QT_TR_NOOP("布局"), QStringLiteral("自由"),      // i18n: value
                     {QStringLiteral("自由"), QStringLiteral("垂直"), QStringLiteral("水平")}), // i18n: value
              spacing},
             80, 60, true),
    };
}

QList<WidgetMeta> displayWidgets()
{
    return {
        meta(QStringLiteral("LEDIndicator"), QT_TR_NOOP("LED指示灯"), WidgetCategory::Display, QStringLiteral("led"),
             140, 30,
             {str(QStringLiteral("label"), QT_TR_NOOP("标签"), QT_TR_NOOP("状态")),
              choice(QStringLiteral("color"), QT_TR_NOOP("颜色"), QStringLiteral("green"),
                     {QStringLiteral("green"), QStringLiteral("red"), QStringLiteral("yellow"), QStringLiteral("blue"),
                      QStringLiteral("gray")}),
              boolean(QStringLiteral("on"), QT_TR_NOOP("点亮"), true, true),
              str(QStringLiteral("onValue"), QT_TR_NOOP("亮值"), QStringLiteral("1")),
              str(QStringLiteral("offValue"), QT_TR_NOOP("灭值"), QStringLiteral("0"))}),
        meta(QStringLiteral("NumericDisplay"), QT_TR_NOOP("数值显示"), WidgetCategory::Display,
             QStringLiteral("seven_segment"), 160, 90,
             {str(QStringLiteral("title"), QT_TR_NOOP("标题"), QT_TR_NOOP("温度")),
              str(QStringLiteral("value"), QT_TR_NOOP("数值"), QStringLiteral("0.00"), true),
              str(QStringLiteral("unit"), QT_TR_NOOP("单位"), QStringLiteral("°C")),
              integer(QStringLiteral("fontSize"), QT_TR_NOOP("字号"), 28, 12, 72),
              choice(QStringLiteral("displayFormat"), QT_TR_NOOP("显示格式"), QStringLiteral("自动"), // i18n: value
                     kDisplayFormats),
              choice(QStringLiteral("decimals"), QT_TR_NOOP("小数位数"),
                     QStringLiteral("自动"), // i18n: value
                     kDecimals)}),
        meta(QStringLiteral("ProgressBar"), QT_TR_NOOP("进度条"), WidgetCategory::Display,
             QStringLiteral("progress_bar"), 250, 20,
             {integer(QStringLiteral("value"), QT_TR_NOOP("当前值"), 50, 0, 100, true),
              integer(QStringLiteral("maximum"), QT_TR_NOOP("最大值"), 100, 1, 99999)},
             30, 10),
        meta(QStringLiteral("ProgressRingDisplay"), QT_TR_NOOP("进度环"), WidgetCategory::Display,
             QStringLiteral("progress_ring"), 120, 130,
             {str(QStringLiteral("label"), QT_TR_NOOP("标签"), QT_TR_NOOP("进度")),
              integer(QStringLiteral("value"), QT_TR_NOOP("当前值"), 0, 0, 100, true),
              integer(QStringLiteral("maximum"), QT_TR_NOOP("最大值"), 100, 1, 99999)},
             80, 80),
        meta(QStringLiteral("ImageDisplay"), QT_TR_NOOP("图片显示"), WidgetCategory::Display, QStringLiteral("photo"),
             200, 150,
             {str(QStringLiteral("imagePath"), QT_TR_NOOP("图片路径"), QString()),
              described(choice(QStringLiteral("scaleMode"), QT_TR_NOOP("缩放方式"),
                               QStringLiteral("适应"),                               // i18n: value
                               {QStringLiteral("适应"), QStringLiteral("拉伸"),      // i18n: value
                                QStringLiteral("原始大小")}),                        // i18n: value
                        QStringLiteral("适应：保持比例放到最大；拉伸：铺满控件框；"  // i18n: ignore
                                       "原始大小：按图片的像素大小，放不下时缩小")), // i18n: ignore
              described(integer(QStringLiteral("rotation"), QT_TR_NOOP("旋转(°)"), 0, 0, 359),
                        QStringLiteral("顺时针，绕中心转；控件框不动，整张图片缩小到放得进框里")), // i18n: ignore
              described(boolean(QStringLiteral("mirrorH"), QT_TR_NOOP("水平镜像"), false),
                        QStringLiteral("左右翻转，在旋转之前")), // i18n: ignore
              described(boolean(QStringLiteral("mirrorV"), QT_TR_NOOP("垂直镜像"), false),
                        QStringLiteral("上下翻转，在旋转之前"))}), // i18n: ignore
    };
}

QList<WidgetMeta> chartWidgets()
{
    const QStringList parseModes = {QStringLiteral("键值匹配"), QStringLiteral("JSON路径"),   // i18n: value
                                    QStringLiteral("HEX字节提取"), QStringLiteral("Modbus")}; // i18n: value
    const QStringList mbTypes = {QStringLiteral("uint16"), QStringLiteral("int16"), QStringLiteral("uint32"),
                                 QStringLiteral("int32"), QStringLiteral("float32")};
    return {
        meta(
            QStringLiteral("RealtimeChart"), QT_TR_NOOP("实时曲线"), WidgetCategory::Chart, QStringLiteral("market"),
            400, 280,
            {str(QStringLiteral("title"), QT_TR_NOOP("标题"), QT_TR_NOOP("实时曲线")),
             integer(QStringLiteral("displayPoints"), QT_TR_NOOP("显示点数"), 200, 10, 10000),
             integer(QStringLiteral("curveCount"), QT_TR_NOOP("曲线数量"), 1, 1, 8),
             integer(QStringLiteral("currentCurve"), QT_TR_NOOP("当前编辑曲线"), 0, 0, 7),
             // Per-curve virtual properties, stored as _curve_{i}_{field}.
             str(QStringLiteral("curveName"), QT_TR_NOOP("曲线名称"), QT_TR_NOOP("曲线1")),
             choice(QStringLiteral("curveParseMode"), QT_TR_NOOP("解析模式"), QStringLiteral("键值匹配"), // i18n: value
                    parseModes),
             str(QStringLiteral("curveKey"), QT_TR_NOOP("匹配键"), QString()),
             str(QStringLiteral("curveJsonPath"), QT_TR_NOOP("JSON路径"), QString()),
             integer(QStringLiteral("curveByteOffset"), QT_TR_NOOP("字节偏移"), 0, 0, 9999),
             choice(QStringLiteral("curveDataType"), QT_TR_NOOP("数据类型(HEX)"), QStringLiteral("float32"),
                    kNumericTypes),
             choice(QStringLiteral("curveByteOrder"), QT_TR_NOOP("字节序(HEX)"),
                    QStringLiteral("大端(BE)"), // i18n: value
                    kByteOrders),
             str(QStringLiteral("curveFrameHeader"), QT_TR_NOOP("帧头(HEX)"), QString()),
             str(QStringLiteral("curveFrameTail"), QT_TR_NOOP("帧尾(HEX)"), QString()),
             choice(QStringLiteral("curveMbArea"), QT_TR_NOOP("寄存器区"), ModbusArea::HoldingRegisters,
                    ModbusArea::options()),
             integer(QStringLiteral("curveRegIndex"), QT_TR_NOOP("寄存器地址"), 0, 0, 65535),
             choice(QStringLiteral("curveMbDataType"), QT_TR_NOOP("数据类型(MB)"), QStringLiteral("uint16"), mbTypes),
             choice(QStringLiteral("curveMbRegOrder"), QT_TR_NOOP("寄存器序(MB)"),
                    QStringLiteral("大端(AB CD)"),                                   // i18n: value
                    {QStringLiteral("大端(AB CD)"), QStringLiteral("小端(CD AB)")}), // i18n: value
             integer(QStringLiteral("curveMbFcFilter"), QT_TR_NOOP("功能码过滤"), -1, -1, 255),
             integer(QStringLiteral("curveMbSlaveFilter"), QT_TR_NOOP("从站过滤"), -1, -1, 255)},
            200, 150),
        meta(QStringLiteral("GaugeMeter"), QT_TR_NOOP("仪表盘"), WidgetCategory::Chart, QStringLiteral("speed_high"),
             180, 180,
             {str(QStringLiteral("title"), QT_TR_NOOP("标题"), QT_TR_NOOP("仪表")),
              str(QStringLiteral("unit"), QT_TR_NOOP("单位"), QString()),
              real(QStringLiteral("value"), QT_TR_NOOP("当前值"), 0.0, -99999, 99999, true),
              real(QStringLiteral("minimum"), QT_TR_NOOP("最小值"), 0.0, -99999, 99999),
              real(QStringLiteral("maximum"), QT_TR_NOOP("最大值"), 100.0, -99999, 99999)},
             80, 80),
        // The Python designer files the data table under charts as well.
        meta(QStringLiteral("DataTable"), QT_TR_NOOP("数据表格"), WidgetCategory::Chart, QStringLiteral("table"), 350,
             250,
             {text(QStringLiteral("columns"), QT_TR_NOOP("列名"), QT_TR_NOOP("字段\n值")),
              choice(QStringLiteral("recordMode"), QT_TR_NOOP("记录方式"), QStringLiteral("最新值"), // i18n: value
                     {QStringLiteral("最新值"), QStringLiteral("逐行记录")}),                        // i18n: value
              integer(QStringLiteral("maxRows"), QT_TR_NOOP("最大行数"), 500, 10, 50000)},
             200, 100),
    };
}

QList<WidgetMeta> commWidgets()
{
    return {
        meta(
            QStringLiteral("SerialConfigCard"), QT_TR_NOOP("串口配置卡片"), WidgetCategory::Comm,
            QStringLiteral("serial_port"), 360, 320,
            QList<PropDef>{
                str(QStringLiteral("title"), QT_TR_NOOP("标题"), QT_TR_NOOP("串口配置")),
                choice(QStringLiteral("displayMode"), QT_TR_NOOP("显示模式"), QStringLiteral("完整"), // i18n: value
                       {QStringLiteral("完整"), QStringLiteral("简洁"), QStringLiteral("单行")})} +   // i18n: value
                linkParsing(true) +
                QList<PropDef>{
                    choice(QStringLiteral("defaultBaud"), QT_TR_NOOP("默认波特率"), QStringLiteral("115200"),
                           {QStringLiteral("9600"), QStringLiteral("19200"), QStringLiteral("38400"),
                            QStringLiteral("57600"), QStringLiteral("115200"), QStringLiteral("230400"),
                            QStringLiteral("460800"), QStringLiteral("921600")}),
                    choice(QStringLiteral("defaultDatabits"), QT_TR_NOOP("默认数据位"), QStringLiteral("8"),
                           {QStringLiteral("5"), QStringLiteral("6"), QStringLiteral("7"), QStringLiteral("8")}),
                    choice(QStringLiteral("defaultParity"), QT_TR_NOOP("默认校验位"), QStringLiteral("None"),
                           {QStringLiteral("None"), QStringLiteral("Even"), QStringLiteral("Odd"),
                            QStringLiteral("Mark"), QStringLiteral("Space")}),
                    choice(QStringLiteral("defaultStopbits"), QT_TR_NOOP("默认停止位"), QStringLiteral("1"),
                           {QStringLiteral("1"), QStringLiteral("1.5"), QStringLiteral("2")}),
                    choice(QStringLiteral("defaultFlow"), QT_TR_NOOP("默认流控制"), QStringLiteral("无"), // i18n: value
                           {QStringLiteral("无"), QStringLiteral("RTS/CTS"), QStringLiteral("XON/XOFF"),  // i18n: value
                            QStringLiteral("DTR/DSR")})},
            200, 50),
        meta(QStringLiteral("TcpConfigCard"), QT_TR_NOOP("TCP 配置卡片"), WidgetCategory::Comm, QStringLiteral("globe"),
             360, 250,
             QList<PropDef>{
                 str(QStringLiteral("title"), QT_TR_NOOP("标题"), QT_TR_NOOP("TCP 配置")),
                 choice(QStringLiteral("displayMode"), QT_TR_NOOP("显示模式"), QStringLiteral("完整"),   // i18n: value
                        {QStringLiteral("完整"), QStringLiteral("简洁")}),                               // i18n: value
                 choice(QStringLiteral("defaultMode"), QT_TR_NOOP("默认模式"), QStringLiteral("客户端"), // i18n: value
                        {QStringLiteral("客户端"), QStringLiteral("服务端")}),                           // i18n: value
                 str(QStringLiteral("defaultHost"), QT_TR_NOOP("默认地址"), QStringLiteral("127.0.0.1")),
                 integer(QStringLiteral("defaultPort"), QT_TR_NOOP("默认端口"), 8080, 1, 65535)} +
                 linkParsing(true),
             240, 50),
        meta(QStringLiteral("UdpConfigCard"), QT_TR_NOOP("UDP 配置卡片"), WidgetCategory::Comm, QStringLiteral("wifi"),
             360, 220,
             QList<PropDef>{
                 str(QStringLiteral("title"), QT_TR_NOOP("标题"), QT_TR_NOOP("UDP 配置")),
                 choice(QStringLiteral("displayMode"), QT_TR_NOOP("显示模式"), QStringLiteral("完整"), // i18n: value
                        {QStringLiteral("完整"), QStringLiteral("简洁")}),                             // i18n: value
                 described(integer(QStringLiteral("defaultLocalPort"), QT_TR_NOOP("默认本地端口"), 0, 0, 65535),
                           QStringLiteral("0 表示由系统选一个空闲端口")), // i18n: ignore
                 described(str(QStringLiteral("defaultHost"), QT_TR_NOOP("默认远程地址"), QStringLiteral("127.0.0.1")),
                           QStringLiteral("留空则回复最后发送方")), // i18n: ignore
                 integer(QStringLiteral("defaultPort"), QT_TR_NOOP("默认远程端口"), 8080, 1, 65535)} +
                 linkParsing(true),
             240, 50),
        meta(QStringLiteral("MqttConfigCard"), QT_TR_NOOP("MQTT 配置卡片"), WidgetCategory::Comm,
             QStringLiteral("cloud"), 380, 390,
             QList<PropDef>{
                 str(QStringLiteral("title"), QT_TR_NOOP("标题"), QT_TR_NOOP("MQTT 配置")),
                 choice(QStringLiteral("displayMode"), QT_TR_NOOP("显示模式"), QStringLiteral("完整"), // i18n: value
                        {QStringLiteral("完整"), QStringLiteral("简洁")}),                             // i18n: value
                 str(QStringLiteral("defaultHost"), QT_TR_NOOP("默认服务器"), QStringLiteral("127.0.0.1")),
                 integer(QStringLiteral("defaultPort"), QT_TR_NOOP("默认端口"), 1883, 1, 65535),
                 str(QStringLiteral("defaultClientId"), QT_TR_NOOP("默认客户端ID"), QString()),
                 str(QStringLiteral("defaultUsername"), QT_TR_NOOP("默认用户名"), QString()),
                 str(QStringLiteral("defaultPassword"), QT_TR_NOOP("默认密码"), QString()),
                 str(QStringLiteral("defaultSubscribe"), QT_TR_NOOP("默认订阅主题"), QStringLiteral("device/data")),
                 str(QStringLiteral("defaultPublish"), QT_TR_NOOP("默认发布主题"), QStringLiteral("device/cmd")),
                 choice(QStringLiteral("defaultQos"), QT_TR_NOOP("默认 QoS"), QStringLiteral("0"),
                        {QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2")}),
                 boolean(QStringLiteral("defaultTls"), QT_TR_NOOP("默认启用 TLS"), false)} +
                 linkParsing(false),
             260, 50),
        // A CAN channel of a PCAN, ZLG, SocketCAN or slcan adapter, decoding with a DBC file and inline signals.
        meta(QStringLiteral("CanConfigCard"), QT_TR_NOOP("CAN 配置卡片"), WidgetCategory::Comm, QStringLiteral("car"),
             380, 270,
             {str(QStringLiteral("title"), QT_TR_NOOP("标题"), QT_TR_NOOP("CAN 连接")),
              choice(QStringLiteral("displayMode"), QT_TR_NOOP("显示模式"), QStringLiteral("完整"), // i18n: value
                     {QStringLiteral("完整"), QStringLiteral("简洁")}),                             // i18n: value
              described(choice(QStringLiteral("defaultAdapter"), QT_TR_NOOP("默认适配器"), QStringLiteral("PCAN"),
                               {QStringLiteral("PCAN"), QStringLiteral("ZLG"), QStringLiteral("SocketCAN"),
                                QStringLiteral("slcan")}),
                        QStringLiteral("PCAN：Windows、macOS；ZLG：Windows；SocketCAN：Linux；slcan：所有系统")), // i18n: ignore
              choice(QStringLiteral("zlgModel"), QT_TR_NOOP("ZLG 型号"), QStringLiteral("USBCANFD-200U"),
                     {QStringLiteral("USBCAN-I"), QStringLiteral("USBCAN-I+"), QStringLiteral("USBCAN-I-mini"),
                      QStringLiteral("USBCAN-II"), QStringLiteral("USBCAN-II+"), QStringLiteral("USBCAN-E-U"),
                      QStringLiteral("USBCAN-2E-U"), QStringLiteral("USBCAN-4E-U"), QStringLiteral("USBCAN-8E-U"),
                      QStringLiteral("USBCANFD-100U-mini"), QStringLiteral("USBCANFD-100U"),
                      QStringLiteral("USBCANFD-200U"), QStringLiteral("USBCANFD-400U"),
                      QStringLiteral("USBCANFD-800U")}),
              described(integer(QStringLiteral("defaultDeviceIndex"), QT_TR_NOOP("ZLG 设备索引"), 0, 0, 15),
                        QStringLiteral("同型号插了多个时的序号，从 0 开始")), // i18n: ignore
              described(integer(QStringLiteral("defaultChannel"), QT_TR_NOOP("ZLG 通道"), 0, 0, 7),
                        QStringLiteral("设备上的第几路 CAN，从 0 开始")), // i18n: ignore
              choice(QStringLiteral("defaultBitrate"), QT_TR_NOOP("默认波特率"), QStringLiteral("500k"),
                     {QStringLiteral("10k"), QStringLiteral("20k"), QStringLiteral("50k"), QStringLiteral("100k"),
                      QStringLiteral("125k"), QStringLiteral("250k"), QStringLiteral("500k"), QStringLiteral("800k"),
                      QStringLiteral("1M")}),
              choice(QStringLiteral("defaultSamplePoint"), QT_TR_NOOP("默认采样点"), QStringLiteral("87.5%"),
                     {QStringLiteral("87.5%"), QStringLiteral("81.25%"), QStringLiteral("80%"), QStringLiteral("75%")}),
              boolean(QStringLiteral("canFd"), QT_TR_NOOP("CAN FD"), false),
              choice(QStringLiteral("defaultDataBitrate"), QT_TR_NOOP("数据域波特率"), QStringLiteral("2M"),
                     {QStringLiteral("1M"), QStringLiteral("2M"), QStringLiteral("4M"), QStringLiteral("5M"),
                      QStringLiteral("8M")}),
              choice(QStringLiteral("defaultDataSamplePoint"), QT_TR_NOOP("数据域采样点"), QStringLiteral("75%"),
                     {QStringLiteral("75%"), QStringLiteral("80%")}),
              described(boolean(QStringLiteral("canBrs"), QT_TR_NOOP("加速 (BRS)"), true),
                        QStringLiteral("CAN FD 报文的数据段用数据域波特率发送")), // i18n: ignore
              described(str(QStringLiteral("customTiming"), QT_TR_NOOP("自定义位时序"), QString()),
                        QStringLiteral("`BRP,TSEG1,TSEG2,SJW`，CAN FD 再加 `;BRP,TSEG1,TSEG2,SJW`（数据域），按适配器" // i18n: ignore
                                       "时钟（PCAN 经典 CAN 8 MHz、CAN FD 80 MHz）；留空则按波特率和采样点计算")),  // i18n: ignore
              boolean(QStringLiteral("listenOnly"), QT_TR_NOOP("只听模式"), false),
              described(boolean(QStringLiteral("termination"), QT_TR_NOOP("终端电阻"), false),
                        QStringLiteral("ZLG USBCANFD 系列可由软件开关，其他适配器忽略")), // i18n: ignore
              described(str(QStringLiteral("dbcFile"), QT_TR_NOOP("DBC 文件"), QString()),
                        QStringLiteral("相对 .uidp 所在文件夹的路径；导出时复制进应用")), // i18n: ignore
              described(text(QStringLiteral("canSignals"), QT_TR_NOOP("CAN 信号"), QString()),
                        QStringLiteral("DBC 之外的信号，每行一个：`ID 名字 : 起始位|长度@1+ (因子,偏移) [最小|最大] " // i18n: ignore
                                       "\"单位\"`，ID 是十六进制，`@1` Intel、`@0` Motorola，`+` 无符号、`-` 有符号")), // i18n: ignore
              described(str(QStringLiteral("canFilter"), QT_TR_NOOP("接收过滤"), QString()),
                        QStringLiteral("留空收全部；否则只收这些 ID，如 `100-1FF, 18FF50E5`")) // i18n: ignore
             },
             240, 50),
        meta(QStringLiteral("DataSendCard"), QT_TR_NOOP("数据发送卡片"), WidgetCategory::Comm, QStringLiteral("send"),
             350, 140,
             {str(QStringLiteral("title"), QT_TR_NOOP("标题"), QT_TR_NOOP("数据发送")),
              str(QStringLiteral("placeholder"), QT_TR_NOOP("提示文字"), QT_TR_NOOP("输入要发送的数据..."))},
             240, 100),
        meta(QStringLiteral("DataRecvCard"), QT_TR_NOOP("数据接收卡片"), WidgetCategory::Comm,
             QStringLiteral("download"), 350, 250,
             {str(QStringLiteral("title"), QT_TR_NOOP("标题"), QT_TR_NOOP("数据接收")),
              integer(QStringLiteral("maxLines"), QT_TR_NOOP("最大行数"), 1000, 50, 50000)},
             240, 120),
        meta(QStringLiteral("LogBox"), QT_TR_NOOP("日志显示框"), WidgetCategory::Comm, QStringLiteral("document"), 400,
             200, {integer(QStringLiteral("maxLines"), QT_TR_NOOP("最大行数"), 500, 50, 50000)}, 200, 80),
        // One row per CAN ID seen on the project's CAN links.
        meta(QStringLiteral("CanMonitor"), QT_TR_NOOP("报文监视"), WidgetCategory::Comm, QStringLiteral("view"), 560,
             280,
             {described(boolean(QStringLiteral("showSignals"), QT_TR_NOOP("显示信号"), false),
                        QStringLiteral("多一列，列出 DBC 和 canSignals 解出的信号值"))}, // i18n: ignore
             240, 100),
        // The data table of the first link card whose modbusRole is 从站, one area of it.
        meta(QStringLiteral("ModbusSlaveTable"), QT_TR_NOOP("从站寄存器表"), WidgetCategory::Comm,
             QStringLiteral("table"), 260, 300,
             {choice(QStringLiteral("mbArea"), QT_TR_NOOP("区"), ModbusArea::HoldingRegisters, ModbusArea::options()),
              described(choice(QStringLiteral("displayFormat"), QT_TR_NOOP("显示格式"),
                               QStringLiteral("无符号"),                            // i18n: value
                               {QStringLiteral("无符号"), QStringLiteral("有符号"), // i18n: value
                                QStringLiteral("十六进制")}),                       // i18n: value
                        QStringLiteral("线圈和离散输入总是显示 0/1"))},             // i18n: ignore
             160, 120),
    };
}

} // namespace

QList<WidgetMeta> builtinWidgets()
{
    QList<WidgetMeta> all;
    all += basicWidgets();
    all += layoutWidgets();
    all += displayWidgets();
    all += chartWidgets();
    all += commWidgets();
    return all;
}

} // namespace fhmi
