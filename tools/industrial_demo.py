"""Build the cross-platform gallery and its repeatable feedback sequence."""

import json
import re


GROUP_NAMES = {
    "instrument": "工业仪表",
    "equipment": "工业设备",
    "automotive": "汽车电子",
    "diy": "电子 DIY",
    "embedded": "嵌入式",
}

SIGNAL_PAGE = (
    ("StatusWordPanel", "machine_status", (12, 42, 307, 190)),
    ("DigitalTimingChart", "machine_status", (329, 42, 357, 190)),
    ("BmsCellMatrix", "cell_voltages", (696, 42, 310, 190)),
    ("RotaryEncoder", "encoder_count", (12, 247, 307, 190)),
    ("DcPowerSupply", "bench_supply", (329, 247, 357, 190)),
    ("DirectionalValve", "directional_valve", (696, 247, 310, 190)),
)

# Visible stage, fraction of the numeric scale, byte supplied to digital displays.
STEPS = (
    ("停机与清零", 0, 0x00),
    ("正向运行 · 水泵启动、空压机加载", .65, 0x01),
    ("反向运行 · 水泵与空压机故障", .35, 0x02),
    ("可逆设备停转 · 空压机卸载", 0, 0x04),
    ("量程下限", 0, 0x08),
    ("量程上限", 1, 0x10),
    ("四分之一量程", .25, 0x20),
    ("四分之三量程", .75, 0x40),
    ("量程中点 · 数码管小数点", .5, 0x80),
    ("低量程 · 数码管显示 0", .1, 0x3F),
    ("高量程 · 数码管显示 1", .9, 0x06),
    ("中量程 · 数码管显示 5", .5, 0x6D),
    ("数码管显示 0 并点亮小数点", .3, 0xBF),
    ("交替位 0x55", .7, 0x55),
    ("交替位 0xAA", .2, 0xAA),
    ("全部标志置位", .8, 0xFF),
)


def data_key(kind):
    return re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", kind).lower()


def geometry(x, y, w, h):
    return dict(x=x, y=y, w=w, h=h)


def bound_widget(kind, widget_id, key, page, position, properties=None):
    widget = dict(type=kind, id=widget_id, tabIndex=page, geometry=position,
                  binding={"parseMode": "键值匹配", "key": key})
    if properties:
        widget["properties"] = properties
    return widget


def gallery_project(rows):
    names, children = [], []
    signal_types = {kind for kind, _, _ in SIGNAL_PAGE}
    for group, name in GROUP_NAMES.items():
        members = [row for row in rows if row["group"] == group and row["type"] not in signal_types]
        for start in range(0, len(members), 12):
            page = start // 12
            if group == "equipment" and page < 2:
                title = ("泵阀与输送", "执行与配电")[page]
            else:
                title = f"{name} {page + 1}" if len(members) > 12 else name
            names.append(title)
            for index, row in enumerate(members[start:start + 12]):
                key = data_key(row["type"])
                position = geometry(12 + index % 4 * 250, 42 + index // 4 * 143, 242, 135)
                children.append(bound_widget(row["type"], key, key, len(names) - 1, position,
                                             {"showLimits": True} if "limits" in row else None))
    names.append("反馈模式")
    variants = [
        ("Motor", "motor_run_bit", "motor_run_bit", dict(title="电机 · 运行位", feedbackMode="运行位")),
        ("Cylinder", "cylinder_ends", "cylinder_ends", dict(title="气缸 · 端位反馈", feedbackMode="端位状态")),
        ("Tank", "tank_without_limits", "tank", dict(title="水箱 · 仅液位", showLimits=False)),
        ("FlowPipe", "pipe_straight", "flow_pipe", dict(title="直管 · 细", pipeShape="直管", pipeWidth=8)),
        ("FlowPipe", "pipe_straight_mirrored", "flow_pipe",
         dict(title="镜像直管 · 粗", pipeShape="直管", pipeDirection="镜像布局", pipeWidth=26)),
        ("FlowPipe", "pipe_elbow_mirrored", "flow_pipe",
         dict(title="镜像弯管", pipeShape="弯管", pipeDirection="镜像布局", pipeWidth=16)),
    ]
    for index, (kind, widget_id, key, properties) in enumerate(variants):
        position = geometry(12 + index % 3 * 333, 42 + index // 3 * 202, 321, 188)
        children.append(bound_widget(kind, widget_id, key, len(names) - 1, position, properties))
    names.append("信号与诊断")
    for kind, key, position in SIGNAL_PAGE:
        props = {"bitProfile": "machine"} if kind in ("StatusWordPanel", "DigitalTimingChart") else None
        widget_id = data_key(kind)
        children.append(bound_widget(kind, widget_id, key, len(names) - 1, geometry(*position), props))
    widgets = [
        dict(type="Label", id="heading", geometry=geometry(20, 9, 285, 30),
             properties={"text": "动态控件全景", "fontSize": 20, "bold": True}),
        dict(type="Label", id="stage", geometry=geometry(315, 10, 725, 29),
             properties={"text": "等待回放", "fontSize": 15}, binding={"parseMode": "键值匹配", "key": "demo_step"}),
        dict(type="TcpConfigCard", id="replay_link", geometry=geometry(20, 48, 1020, 174),
             properties={"title": "动态回放", "defaultHost": "127.0.0.1", "defaultPort": 8766,
                         "defaultParser": "line", "displayMode": "简洁"}),
        dict(type="TabWidget", id="gallery", geometry=geometry(20, 236, 1020, 500),
             properties={"tabNames": "\n".join(names), "currentTab": 0, "tabLayout": "自由"}, children=children),
    ]
    bits = [{"name": name, "color": color} for name, color in (
        ("就绪", "#24895F"), ("运行", "#007CB0"), ("过温", "#CD641E"), ("停机", "#BF3850"),
        ("缺料", "#B68A13"), ("过流", "#AB406A"), ("联锁", "#72539A"), ("维护", "#49718E"))]
    return {"version": "1.0", "appName": "动态控件全景", "canvas": {"width": 1060, "height": 756, "gridSize": 10},
            "bitProfiles": {"machine": {"width": 8, "bits": bits}},
            "serial": {"parser": "line", "parserConfig": {}}, "widgets": widgets}


def sample_value(row, step, fraction, byte):
    mode, low, high = row["mode"], row["minimum"], row["maximum"]
    if mode == "byte":
        return byte & high
    if mode in ("state", "bits", "flags"):
        return step % (high + 1)
    if step in (0, 3):
        value = 0 if low <= 0 <= high else low
    elif step in (1, 2) and low < 0 < high:
        value = .65 * (high if step == 1 else low)
    else:
        value = low + (high - low) * fraction
    return round(value, row["decimals"]) if row["decimals"] else round(value)


def gallery_frames(rows):
    signals = {kind for kind, _, _ in SIGNAL_PAGE}
    singles = [row for row in rows if row["type"] not in signals]
    words = (1, 3, 7, 13, 9, 1, 3, 19, 35, 43, 65, 129, 193, 85, 170, 255)
    counts = (0, 256, 512, 512, 256, -128, -512, 0, 1024, 1536, 1792, 2048, 1920, 1984, 2048, 2304)
    cell_mv = (0, 12, -5, 7, -2, 4, 10, 3)
    supplies = ((12, .15, "CV"), (12, .48, "CV"), (11.7, 1.0, "CC"), (10.8, 1.0, "CC"),
                (9.5, 1.0, "CC"), (12, .62, "CV"), (12, .25, "CV"), (12, .09, "CV"))
    sequence = []
    for i, (name, fraction, byte) in enumerate(STEPS):
        values = {data_key(row["type"]): sample_value(row, i, fraction, byte) for row in singles}
        volts = [round((3680 + 2 * i + cell_mv[(n + i) % len(cell_mv)]) / 1000, 3) for n in range(len(cell_mv))]
        v, a, mode = supplies[i % len(supplies)]
        values.update(machine_status=words[i], encoder_count=counts[i], cell_voltages=volts,
                      bench_supply={"voltage": v, "current": a, "mode": mode}, directional_valve=i % 3)
        values.update(motor_run_bit=i % 2, cylinder_ends=i % 3)
        sequence.append((name, values))
    invalid = {data_key(row["type"]): "invalid" if row["mode"] == "number" else row["maximum"] + 1 for row in singles}
    invalid.update(motor_run_bit=2, cylinder_ends=3, machine_status=65536, encoder_count=1.5,
                   cell_voltages=[], bench_supply={"voltage": 12, "mode": "CV"}, directional_valve=3)
    sequence.append(("无效反馈 · 检查异常提示与停转", invalid))
    restored = {data_key(row["type"]): row["value"] for row in singles}
    restored.update(motor_run_bit=1, cylinder_ends=2, machine_status=3, encoder_count=256,
                    cell_voltages=[3.70] * 8, bench_supply={"voltage": 12, "current": .425, "mode": "CV"},
                    directional_valve=1)
    sequence.append(("恢复有效反馈", restored))
    return "".join(json.dumps({"demo_step": f"{i:02}/{len(sequence)} · {name}", **values},
                              ensure_ascii=False, separators=(",", ":")) + "\n"
                   for i, (name, values) in enumerate(sequence, 1))
