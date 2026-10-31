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

// A link card's own frame format: the keys of the project's serial.parserConfig.
PropDef parserConfig()
{
    return PropDef{QStringLiteral("parserConfig"), QStringLiteral("帧格式"), PropType::Object, QJsonObject(), {},
                   std::nullopt, std::nullopt, false,
                   QStringLiteral("帧格式设置，键同工程的 serial.parserConfig；默认解析模式不是「项目设置」时生效，"
                                  "空对象表示沿用工程设置（模式相同时）或默认值")};
}

const QStringList kCommandModes = {QStringLiteral("文本"), QStringLiteral("HEX"), QStringLiteral("Modbus")};
const QStringList kHexTypes = {QStringLiteral("HEX字符串"), QStringLiteral("uint8"),  QStringLiteral("int8"),
                               QStringLiteral("uint16"),    QStringLiteral("int16"),  QStringLiteral("uint32"),
                               QStringLiteral("int32"),     QStringLiteral("float32")};
const QStringList kNumericTypes = {QStringLiteral("uint8"),  QStringLiteral("int8"),  QStringLiteral("uint16"),
                                   QStringLiteral("int16"),  QStringLiteral("uint32"), QStringLiteral("int32"),
                                   QStringLiteral("float32")};
const QStringList kByteOrders = {QStringLiteral("大端(BE)"), QStringLiteral("小端(LE)")};
const QStringList kNewlines = {QStringLiteral("无"), QStringLiteral("\\r\\n"), QStringLiteral("\\n"),
                               QStringLiteral("\\r")};
const QStringList kAllFunctions = CommandEncoder::functionOptions();
const QStringList kInputFunctions = CommandEncoder::functionOptions({0x03, 0x04, 0x05, 0x06, 0x10});
const QStringList kWriteFunctions = CommandEncoder::functionOptions({0x05, 0x06, 0x10});
const QStringList kNotifyTypes = {QStringLiteral("成功"), QStringLiteral("信息"), QStringLiteral("警告"),
                                  QStringLiteral("错误")};
const QStringList kDisplayFormats = {QStringLiteral("自动"), QStringLiteral("十进制"),
                                     QStringLiteral("十六进制(HEX)"), QStringLiteral("二进制(BIN)")};
const QStringList kDecimals = {QStringLiteral("自动"), QStringLiteral("0"), QStringLiteral("1"),
                               QStringLiteral("2"),    QStringLiteral("3"), QStringLiteral("4")};
const QStringList kLinkParsers = {QStringLiteral("项目设置"), QStringLiteral("line"), QStringLiteral("hex"),
                                  QStringLiteral("modbus"), QStringLiteral("modbus_tcp"),
                                  QStringLiteral("modbus_ascii")};

// The framing fields every command-sending widget shares, between the mode and Modbus.
QList<PropDef> hexFraming(const QStringList &dataTypes, const QString &dataTypeDefault, bool newlineFirst)
{
    QList<PropDef> out{str(QStringLiteral("frameHeader"), QStringLiteral("帧头(HEX)"), QString()),
                       str(QStringLiteral("frameTail"), QStringLiteral("帧尾(HEX)"), QString())};
    const PropDef newline = choice(QStringLiteral("commandNewline"), QStringLiteral("换行符"),
                                   QStringLiteral("\\r\\n"), kNewlines);
    if (newlineFirst) {
        out.append(newline);
    }
    out.append(choice(QStringLiteral("dataType"), QStringLiteral("数据类型"), dataTypeDefault, dataTypes));
    out.append(choice(QStringLiteral("byteOrder"), QStringLiteral("字节序"), QStringLiteral("大端(BE)"), kByteOrders));
    if (!newlineFirst) {
        out.append(newline);
    }
    return out;
}

QList<PropDef> notification(const QString &event, const QString &content, const QString &type)
{
    return {boolean(QStringLiteral("notify"), event, false),
            str(QStringLiteral("notifyTitle"), QStringLiteral("通知标题"), QStringLiteral("提示")),
            str(QStringLiteral("notifyContent"), QStringLiteral("通知内容"), content),
            choice(QStringLiteral("notifyType"), QStringLiteral("通知类型"), type, kNotifyTypes),
            integer(QStringLiteral("notifyDuration"), QStringLiteral("通知时长(ms)"), 3000, 500, 30000)};
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
    const PropDef mode = choice(QStringLiteral("commandMode"), QStringLiteral("发送模式"), QStringLiteral("文本"),
                                kCommandModes);
    const PropDef tpl = str(QStringLiteral("commandTemplate"), QStringLiteral("发送模板"), QStringLiteral("{value}"));
    const PropDef slave = integer(QStringLiteral("mbSlave"), QStringLiteral("从站地址"), 1, 1, 247);
    const PropDef address = integer(QStringLiteral("mbAddress"), QStringLiteral("寄存器地址"), 0, 0, 65535);
    // A count for reads, a whole number to write (negative for int16 / int32) otherwise.
    const PropDef mbValue = integer(QStringLiteral("mbValue"), QStringLiteral("值/数量"), 1, -2147483648.0, 2147483647.0);
    // How a written value fills registers: FC 06 takes a 16-bit type, FC 16 any of them.
    const PropDef dataType = choice(QStringLiteral("mbDataType"), QStringLiteral("数据类型"), QStringLiteral("uint16"),
                                    CommandEncoder::dataTypes());
    const PropDef regOrder = choice(QStringLiteral("mbRegOrder"), QStringLiteral("寄存器序"),
                                    CommandEncoder::registerOrders().first(), CommandEncoder::registerOrders());

    QList<PropDef> button{str(QStringLiteral("text"), QStringLiteral("文本"), QStringLiteral("按钮")),
                          boolean(QStringLiteral("enabled"), QStringLiteral("启用"), true),
                          mode,
                          command(QStringLiteral("command"), QStringLiteral("发送内容")),
                          tpl};
    button += hexFraming(kHexTypes, QStringLiteral("HEX字符串"), false);
    button += {slave,
               choice(QStringLiteral("mbFunc"), QStringLiteral("功能码"), QStringLiteral("03 读保持寄存器"),
                      kAllFunctions),
               address, dataType, regOrder, mbValue};
    button += notification(QStringLiteral("按下通知"), QStringLiteral("按钮已按下"), QStringLiteral("成功"));

    QList<PropDef> label{str(QStringLiteral("text"), QStringLiteral("文本"), QStringLiteral("标签"), true),
                         integer(QStringLiteral("fontSize"), QStringLiteral("字号"), 14, 8, 72),
                         boolean(QStringLiteral("bold"), QStringLiteral("加粗"), false),
                         choice(QStringLiteral("alignment"), QStringLiteral("对齐"), QStringLiteral("Left"),
                                {QStringLiteral("Left"), QStringLiteral("Center"), QStringLiteral("Right")}),
                         choice(QStringLiteral("displayFormat"), QStringLiteral("显示格式"), QStringLiteral("自动"),
                                kDisplayFormats),
                         choice(QStringLiteral("decimals"), QStringLiteral("小数位数"), QStringLiteral("自动"),
                                kDecimals)};

    QList<PropDef> lineEdit{str(QStringLiteral("placeholderText"), QStringLiteral("占位文本"),
                                QStringLiteral("请输入...")),
                            str(QStringLiteral("text"), QStringLiteral("默认值"), QString()),
                            boolean(QStringLiteral("readOnly"), QStringLiteral("只读"), false),
                            boolean(QStringLiteral("sendOnEnter"), QStringLiteral("回车发送"), false),
                            mode,
                            tpl};
    lineEdit += hexFraming(kHexTypes, QStringLiteral("HEX字符串"), false);
    lineEdit += {slave,
                 choice(QStringLiteral("mbFunc"), QStringLiteral("功能码"), QStringLiteral("06 写单寄存器"),
                        kInputFunctions),
                 address, dataType, regOrder};

    QList<PropDef> combo{text(QStringLiteral("items"), QStringLiteral("选项"), QStringLiteral("选项1\n选项2\n选项3")),
                         integer(QStringLiteral("currentIndex"), QStringLiteral("默认选项"), 0, 0, 99),
                         boolean(QStringLiteral("sendOnSelect"), QStringLiteral("选中发送"), false),
                         text(QStringLiteral("commands"), QStringLiteral("发送命令表"), QString()),
                         mode,
                         tpl};
    combo += hexFraming(kHexTypes, QStringLiteral("HEX字符串"), false);
    combo += {slave,
              choice(QStringLiteral("mbFunc"), QStringLiteral("功能码"), QStringLiteral("06 写单寄存器"),
                     kInputFunctions),
              address, dataType, regOrder, mbValue};

    QList<PropDef> toggle{str(QStringLiteral("label"), QStringLiteral("标签"), QStringLiteral("开关")),
                          boolean(QStringLiteral("checked"), QStringLiteral("默认开启"), false),
                          mode,
                          command(QStringLiteral("onCommand"), QStringLiteral("开启命令")),
                          command(QStringLiteral("offCommand"), QStringLiteral("关闭命令")),
                          tpl};
    toggle += hexFraming(kHexTypes, QStringLiteral("HEX字符串"), false);
    toggle += {slave, integer(QStringLiteral("mbAddress"), QStringLiteral("线圈地址"), 0, 0, 65535)};
    toggle += notification(QStringLiteral("切换通知"), QStringLiteral("开关已切换"), QStringLiteral("信息"));

    const auto ranged = [&](int value, bool suffix) {
        QList<PropDef> props{integer(QStringLiteral("minimum"), QStringLiteral("最小值"), 0, -99999, 99999),
                             integer(QStringLiteral("maximum"), QStringLiteral("最大值"), 100, -99999, 99999),
                             integer(QStringLiteral("value"), QStringLiteral("默认值"), value, -99999, 99999)};
        if (suffix) {
            props.append(str(QStringLiteral("suffix"), QStringLiteral("后缀"), QString()));
        }
        props += {boolean(QStringLiteral("sendOnChange"), QStringLiteral("变化发送"), false), mode, tpl};
        props += hexFraming(kNumericTypes, QStringLiteral("uint16"), true);
        props += {slave,
                  choice(QStringLiteral("mbFunc"), QStringLiteral("功能码"), QStringLiteral("06 写单寄存器"),
                         kWriteFunctions),
                  address, dataType, regOrder};
        return props;
    };

    return {meta(QStringLiteral("PushButton"), QStringLiteral("按钮"), WidgetCategory::Basic, QStringLiteral("send"),
                 120, 36, button),
            meta(QStringLiteral("Label"), QStringLiteral("标签"), WidgetCategory::Basic, QStringLiteral("font"), 120,
                 30, label),
            meta(QStringLiteral("LineEdit"), QStringLiteral("输入框"), WidgetCategory::Basic, QStringLiteral("edit"),
                 200, 36, lineEdit),
            meta(QStringLiteral("ComboBox"), QStringLiteral("下拉框"), WidgetCategory::Basic, QStringLiteral("menu"),
                 160, 36, combo),
            meta(QStringLiteral("ToggleSwitch"), QStringLiteral("开关"), WidgetCategory::Basic,
                 QStringLiteral("power_button"), 140, 30, toggle),
            meta(QStringLiteral("Slider"), QStringLiteral("滑块"), WidgetCategory::Basic, QStringLiteral("scroll"), 200,
                 30, ranged(50, false)),
            meta(QStringLiteral("SpinBox"), QStringLiteral("数值框"), WidgetCategory::Basic, QStringLiteral("setting"),
                 140, 36, ranged(0, true))};
}

QList<WidgetMeta> layoutWidgets()
{
    const PropDef spacing = integer(QStringLiteral("spacing"), QStringLiteral("间距"), 8, 0, 100);
    return {
        meta(QStringLiteral("HBoxLayout"), QStringLiteral("水平布局"), WidgetCategory::Layout,
             QStringLiteral("right_arrow"), 300, 80,
             {str(QStringLiteral("title"), QStringLiteral("标题"), QStringLiteral("水平布局")), spacing}, 60, 40, true),
        meta(QStringLiteral("VBoxLayout"), QStringLiteral("垂直布局"), WidgetCategory::Layout,
             QStringLiteral("arrow_down"), 200, 200,
             {str(QStringLiteral("title"), QStringLiteral("标题"), QStringLiteral("垂直布局")), spacing}, 60, 40, true),
        meta(QStringLiteral("FlexLayout"), QStringLiteral("弹性布局"), WidgetCategory::Layout, QStringLiteral("layout"),
             400, 200,
             {str(QStringLiteral("title"), QStringLiteral("标题"), QStringLiteral("弹性布局")),
              choice(QStringLiteral("direction"), QStringLiteral("方向"), QStringLiteral("row"),
                     {QStringLiteral("row"), QStringLiteral("column")}),
              spacing},
             100, 60, true),
        meta(QStringLiteral("GroupBox"), QStringLiteral("分组框"), WidgetCategory::Layout, QStringLiteral("folder"), 280,
             180, {str(QStringLiteral("title"), QStringLiteral("标题"), QStringLiteral("分组"))}, 80, 60, true),
        meta(QStringLiteral("TabWidget"), QStringLiteral("选项卡"), WidgetCategory::Layout, QStringLiteral("tiles"), 350,
             250,
             {text(QStringLiteral("tabNames"), QStringLiteral("标签名称"), QStringLiteral("标签1\n标签2\n标签3")),
              integer(QStringLiteral("currentTab"), QStringLiteral("当前标签页"), 0, 0, 9999),
              choice(QStringLiteral("tabLayout"), QStringLiteral("当前页布局"), QStringLiteral("自由"),
                     {QStringLiteral("自由"), QStringLiteral("垂直"), QStringLiteral("水平"),
                      QStringLiteral("弹性(Flex)")})},
             150, 80, true),
        meta(QStringLiteral("Separator"), QStringLiteral("分割线"), WidgetCategory::Layout, QStringLiteral("remove"),
             200, 2,
             {choice(QStringLiteral("orientation"), QStringLiteral("方向"), QStringLiteral("horizontal"),
                     {QStringLiteral("horizontal"), QStringLiteral("vertical")})},
             20, 2),
        meta(QStringLiteral("CardContainer"), QStringLiteral("卡片容器"), WidgetCategory::Layout,
             QStringLiteral("label"), 300, 200,
             {str(QStringLiteral("title"), QStringLiteral("标题"), QString()),
              choice(QStringLiteral("layout"), QStringLiteral("布局"), QStringLiteral("自由"),
                     {QStringLiteral("自由"), QStringLiteral("垂直"), QStringLiteral("水平")}),
              spacing},
             80, 60, true),
    };
}

QList<WidgetMeta> displayWidgets()
{
    return {
        meta(QStringLiteral("LEDIndicator"), QStringLiteral("LED指示灯"), WidgetCategory::Display,
             QStringLiteral("calories"), 140, 30,
             {str(QStringLiteral("label"), QStringLiteral("标签"), QStringLiteral("状态")),
              choice(QStringLiteral("color"), QStringLiteral("颜色"), QStringLiteral("green"),
                     {QStringLiteral("green"), QStringLiteral("red"), QStringLiteral("yellow"), QStringLiteral("blue"),
                      QStringLiteral("gray")}),
              boolean(QStringLiteral("on"), QStringLiteral("点亮"), true, true),
              str(QStringLiteral("onValue"), QStringLiteral("亮值"), QStringLiteral("1")),
              str(QStringLiteral("offValue"), QStringLiteral("灭值"), QStringLiteral("0"))}),
        meta(QStringLiteral("NumericDisplay"), QStringLiteral("数值显示"), WidgetCategory::Display,
             QStringLiteral("speed_high"), 160, 90,
             {str(QStringLiteral("title"), QStringLiteral("标题"), QStringLiteral("温度")),
              str(QStringLiteral("value"), QStringLiteral("数值"), QStringLiteral("0.00"), true),
              str(QStringLiteral("unit"), QStringLiteral("单位"), QStringLiteral("°C")),
              integer(QStringLiteral("fontSize"), QStringLiteral("字号"), 28, 12, 72),
              choice(QStringLiteral("displayFormat"), QStringLiteral("显示格式"), QStringLiteral("自动"),
                     kDisplayFormats),
              choice(QStringLiteral("decimals"), QStringLiteral("小数位数"), QStringLiteral("自动"), kDecimals)}),
        meta(QStringLiteral("ProgressBar"), QStringLiteral("进度条"), WidgetCategory::Display,
             QStringLiteral("speed_medium"), 250, 20,
             {integer(QStringLiteral("value"), QStringLiteral("当前值"), 50, 0, 100, true),
              integer(QStringLiteral("maximum"), QStringLiteral("最大值"), 100, 1, 99999)},
             30, 10),
        meta(QStringLiteral("ImageDisplay"), QStringLiteral("图片显示"), WidgetCategory::Display,
             QStringLiteral("photo"), 200, 150, {str(QStringLiteral("imagePath"), QStringLiteral("图片路径"), QString())}),
    };
}

QList<WidgetMeta> chartWidgets()
{
    const QStringList parseModes = {QStringLiteral("键值匹配"), QStringLiteral("JSON路径"),
                                    QStringLiteral("HEX字节提取"), QStringLiteral("Modbus")};
    const QStringList mbTypes = {QStringLiteral("uint16"), QStringLiteral("int16"), QStringLiteral("uint32"),
                                 QStringLiteral("int32"), QStringLiteral("float32")};
    return {
        meta(QStringLiteral("RealtimeChart"), QStringLiteral("实时曲线"), WidgetCategory::Chart,
             QStringLiteral("history"), 400, 280,
             {str(QStringLiteral("title"), QStringLiteral("标题"), QStringLiteral("实时曲线")),
              integer(QStringLiteral("displayPoints"), QStringLiteral("显示点数"), 200, 10, 10000),
              integer(QStringLiteral("curveCount"), QStringLiteral("曲线数量"), 1, 1, 8),
              integer(QStringLiteral("currentCurve"), QStringLiteral("当前编辑曲线"), 0, 0, 7),
              // Per-curve virtual properties, stored as _curve_{i}_{field}.
              str(QStringLiteral("curveName"), QStringLiteral("曲线名称"), QStringLiteral("曲线1")),
              choice(QStringLiteral("curveParseMode"), QStringLiteral("解析模式"), QStringLiteral("键值匹配"),
                     parseModes),
              str(QStringLiteral("curveKey"), QStringLiteral("匹配键"), QString()),
              str(QStringLiteral("curveJsonPath"), QStringLiteral("JSON路径"), QString()),
              integer(QStringLiteral("curveByteOffset"), QStringLiteral("字节偏移"), 0, 0, 9999),
              choice(QStringLiteral("curveDataType"), QStringLiteral("数据类型(HEX)"), QStringLiteral("float32"),
                     kNumericTypes),
              choice(QStringLiteral("curveByteOrder"), QStringLiteral("字节序(HEX)"), QStringLiteral("大端(BE)"),
                     kByteOrders),
              str(QStringLiteral("curveFrameHeader"), QStringLiteral("帧头(HEX)"), QString()),
              str(QStringLiteral("curveFrameTail"), QStringLiteral("帧尾(HEX)"), QString()),
              choice(QStringLiteral("curveMbArea"), QStringLiteral("寄存器区"), ModbusArea::InResponse,
                     ModbusArea::options()),
              integer(QStringLiteral("curveRegIndex"), QStringLiteral("寄存器索引/地址"), 0, 0, 65535),
              choice(QStringLiteral("curveMbDataType"), QStringLiteral("数据类型(MB)"), QStringLiteral("uint16"),
                     mbTypes),
              choice(QStringLiteral("curveMbRegOrder"), QStringLiteral("寄存器序(MB)"), QStringLiteral("大端(AB CD)"),
                     {QStringLiteral("大端(AB CD)"), QStringLiteral("小端(CD AB)")}),
              integer(QStringLiteral("curveMbFcFilter"), QStringLiteral("功能码过滤"), -1, -1, 255),
              integer(QStringLiteral("curveMbSlaveFilter"), QStringLiteral("从站过滤"), -1, -1, 255)},
             200, 150),
        meta(QStringLiteral("GaugeMeter"), QStringLiteral("仪表盘"), WidgetCategory::Chart,
             QStringLiteral("speed_high"), 180, 180,
             {str(QStringLiteral("title"), QStringLiteral("标题"), QStringLiteral("仪表")),
              str(QStringLiteral("unit"), QStringLiteral("单位"), QString()),
              real(QStringLiteral("value"), QStringLiteral("当前值"), 0.0, -99999, 99999, true),
              real(QStringLiteral("minimum"), QStringLiteral("最小值"), 0.0, -99999, 99999),
              real(QStringLiteral("maximum"), QStringLiteral("最大值"), 100.0, -99999, 99999)},
             80, 80),
        // The Python designer files the data table under charts as well.
        meta(QStringLiteral("DataTable"), QStringLiteral("数据表格"), WidgetCategory::Chart, QStringLiteral("view"), 350,
             250,
             {text(QStringLiteral("columns"), QStringLiteral("列名"), QStringLiteral("字段\n值")),
              choice(QStringLiteral("recordMode"), QStringLiteral("记录方式"), QStringLiteral("最新值"),
                     {QStringLiteral("最新值"), QStringLiteral("逐行记录")}),
              integer(QStringLiteral("maxRows"), QStringLiteral("最大行数"), 500, 10, 50000)},
             200, 100),
    };
}

QList<WidgetMeta> compositeWidgets()
{
    return {
        meta(QStringLiteral("SerialConfigCard"), QStringLiteral("串口配置卡片"), WidgetCategory::Composite,
             QStringLiteral("connect"), 360, 340,
             {str(QStringLiteral("title"), QStringLiteral("标题"), QStringLiteral("串口配置")),
              choice(QStringLiteral("displayMode"), QStringLiteral("显示模式"), QStringLiteral("完整"),
                     {QStringLiteral("完整"), QStringLiteral("简洁"), QStringLiteral("单行")}),
              choice(QStringLiteral("defaultParser"), QStringLiteral("默认解析模式"), QStringLiteral("项目设置"),
                     kLinkParsers),
              parserConfig(),
              choice(QStringLiteral("defaultBaud"), QStringLiteral("默认波特率"), QStringLiteral("115200"),
                     {QStringLiteral("9600"), QStringLiteral("19200"), QStringLiteral("38400"),
                      QStringLiteral("57600"), QStringLiteral("115200"), QStringLiteral("230400"),
                      QStringLiteral("460800"), QStringLiteral("921600")}),
              choice(QStringLiteral("defaultDatabits"), QStringLiteral("默认数据位"), QStringLiteral("8"),
                     {QStringLiteral("5"), QStringLiteral("6"), QStringLiteral("7"), QStringLiteral("8")}),
              choice(QStringLiteral("defaultParity"), QStringLiteral("默认校验位"), QStringLiteral("None"),
                     {QStringLiteral("None"), QStringLiteral("Even"), QStringLiteral("Odd"), QStringLiteral("Mark"),
                      QStringLiteral("Space")}),
              choice(QStringLiteral("defaultStopbits"), QStringLiteral("默认停止位"), QStringLiteral("1"),
                     {QStringLiteral("1"), QStringLiteral("1.5"), QStringLiteral("2")}),
              choice(QStringLiteral("defaultFlow"), QStringLiteral("默认流控制"), QStringLiteral("无"),
                     {QStringLiteral("无"), QStringLiteral("RTS/CTS"), QStringLiteral("XON/XOFF"),
                      QStringLiteral("DTR/DSR")})},
             200, 50),
        meta(QStringLiteral("TcpConfigCard"), QStringLiteral("TCP 配置卡片"), WidgetCategory::Composite,
             QStringLiteral("globe"), 360, 290,
             {str(QStringLiteral("title"), QStringLiteral("标题"), QStringLiteral("TCP 配置")),
              choice(QStringLiteral("displayMode"), QStringLiteral("显示模式"), QStringLiteral("完整"),
                     {QStringLiteral("完整"), QStringLiteral("简洁")}),
              choice(QStringLiteral("defaultMode"), QStringLiteral("默认模式"), QStringLiteral("客户端"),
                     {QStringLiteral("客户端"), QStringLiteral("服务端")}),
              str(QStringLiteral("defaultHost"), QStringLiteral("默认地址"), QStringLiteral("127.0.0.1")),
              integer(QStringLiteral("defaultPort"), QStringLiteral("默认端口"), 8080, 1, 65535),
              choice(QStringLiteral("defaultParser"), QStringLiteral("默认解析模式"), QStringLiteral("项目设置"),
                     kLinkParsers),
              parserConfig()},
             240, 50),
        meta(QStringLiteral("MqttConfigCard"), QStringLiteral("MQTT 配置卡片"), WidgetCategory::Composite,
             QStringLiteral("cloud"), 380, 430,
             {str(QStringLiteral("title"), QStringLiteral("标题"), QStringLiteral("MQTT 配置")),
              choice(QStringLiteral("displayMode"), QStringLiteral("显示模式"), QStringLiteral("完整"),
                     {QStringLiteral("完整"), QStringLiteral("简洁")}),
              str(QStringLiteral("defaultHost"), QStringLiteral("默认服务器"), QStringLiteral("127.0.0.1")),
              integer(QStringLiteral("defaultPort"), QStringLiteral("默认端口"), 1883, 1, 65535),
              str(QStringLiteral("defaultClientId"), QStringLiteral("默认客户端ID"), QString()),
              str(QStringLiteral("defaultUsername"), QStringLiteral("默认用户名"), QString()),
              str(QStringLiteral("defaultPassword"), QStringLiteral("默认密码"), QString()),
              str(QStringLiteral("defaultSubscribe"), QStringLiteral("默认订阅主题"), QStringLiteral("device/data")),
              str(QStringLiteral("defaultPublish"), QStringLiteral("默认发布主题"), QStringLiteral("device/cmd")),
              choice(QStringLiteral("defaultQos"), QStringLiteral("默认 QoS"), QStringLiteral("0"),
                     {QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2")}),
              boolean(QStringLiteral("defaultTls"), QStringLiteral("默认启用 TLS"), false),
              choice(QStringLiteral("defaultParser"), QStringLiteral("默认解析模式"), QStringLiteral("项目设置"),
                     kLinkParsers),
              parserConfig()},
             260, 50),
        meta(QStringLiteral("ModbusConfigCard"), QStringLiteral("Modbus 配置卡片"), WidgetCategory::Composite,
             QStringLiteral("developer_tools"), 380, 180,
             {str(QStringLiteral("title"), QStringLiteral("标题"), QStringLiteral("Modbus 配置")),
              integer(QStringLiteral("defaultSlave"), QStringLiteral("默认从站"), 1, 1, 247),
              choice(QStringLiteral("defaultFunc"), QStringLiteral("默认功能码"), QStringLiteral("03 读保持寄存器"),
                     kAllFunctions),
              integer(QStringLiteral("defaultAddr"), QStringLiteral("默认地址"), 0, 0, 65535),
              str(QStringLiteral("defaultValue"), QStringLiteral("默认值/数量"), QStringLiteral("1")), // or a decimal
              integer(QStringLiteral("defaultPoll"), QStringLiteral("默认轮询(ms)"), 0, 0, 60000),
              choice(QStringLiteral("mbDataType"), QStringLiteral("写入数据类型"), QStringLiteral("uint16"),
                     CommandEncoder::dataTypes()),
              choice(QStringLiteral("mbRegOrder"), QStringLiteral("写入寄存器序"),
                     CommandEncoder::registerOrders().first(), CommandEncoder::registerOrders()),
              integer(QStringLiteral("timeout"), QStringLiteral("响应超时(ms)"), 1000, 10, 60000),
              choice(QStringLiteral("pollMode"), QStringLiteral("轮询方式"), QStringLiteral("自动"),
                     {QStringLiteral("自动"), QStringLiteral("轮询表")}),
              boolean(QStringLiteral("autoStart"), QStringLiteral("连接后自动轮询"), true),
              text(QStringLiteral("pollList"), QStringLiteral("轮询表"), QString())},
             280, 120),
        meta(QStringLiteral("DataSendCard"), QStringLiteral("数据发送卡片"), WidgetCategory::Composite,
             QStringLiteral("send"), 350, 140,
             {str(QStringLiteral("title"), QStringLiteral("标题"), QStringLiteral("数据发送")),
              str(QStringLiteral("placeholder"), QStringLiteral("提示文字"), QStringLiteral("输入要发送的数据..."))},
             240, 100),
        meta(QStringLiteral("DataRecvCard"), QStringLiteral("数据接收卡片"), WidgetCategory::Composite,
             QStringLiteral("download"), 350, 250,
             {str(QStringLiteral("title"), QStringLiteral("标题"), QStringLiteral("数据接收")),
              integer(QStringLiteral("maxLines"), QStringLiteral("最大行数"), 1000, 50, 50000)},
             240, 120),
        meta(QStringLiteral("LogBox"), QStringLiteral("日志显示框"), WidgetCategory::Composite,
             QStringLiteral("history"), 400, 200,
             {integer(QStringLiteral("maxLines"), QStringLiteral("最大行数"), 500, 50, 50000)}, 200, 80),
        meta(QStringLiteral("ProgressRingDisplay"), QStringLiteral("进度环"), WidgetCategory::Composite,
             QStringLiteral("speed_medium"), 120, 130,
             {str(QStringLiteral("label"), QStringLiteral("标签"), QStringLiteral("进度")),
              integer(QStringLiteral("value"), QStringLiteral("当前值"), 0, 0, 100, true),
              integer(QStringLiteral("maximum"), QStringLiteral("最大值"), 100, 1, 99999)},
             80, 80),
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
    all += compositeWidgets();
    return all;
}

} // namespace fhmi
