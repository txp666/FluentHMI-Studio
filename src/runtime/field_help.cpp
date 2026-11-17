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
