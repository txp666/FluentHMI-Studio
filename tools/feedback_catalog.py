"""Shared drawings and defaults for signal, measurement and valve feedback."""

from widget_art import ellipse, label, line, number, panel, path, rect, state, tag, token, transform


def text_at(x, y, value, size, color="ink", anchor="middle"):
    return tag("text", value, x=x, y=y, fill=token(color), font_family="sans-serif",
               font_size=size, text_anchor=anchor, stroke="none")


def word_tile():
    lamp = ellipse(12, 16, 5, 5, "surface", "muted")
    lit = tag("g", ellipse(12, 16, 5, 5, "color", None), opacity=token("lit"))
    return tag("g", rect(0, 0, token("w"), 32, "surface", "muted", 4) + lamp + lit
               + text_at(23, 20, token("name"), token("nameSize"), anchor="start"),
               transform="translate({{x}} {{y}})", data_role="bit")


def timing_lane():
    return tag("g", line("M82 {{h}}H310", "body", .7)
               + text_at(8, token("labelY"), token("name"), token("fontSize"), anchor="start")
               + line(token("trace"), "color", 1.6),
               transform="translate(0 {{y}})", data_role="lane")


def battery_cell():
    return tag("g", rect(0, 0, token("w"), token("h"), "surface", "outline", 3)
               + text_at(token("cx"), token("nameY"), token("name"), token("nameSize"), "muted")
               + text_at(token("cx"), token("valueY"), token("valueText"), token("valueSize")),
               transform="translate({{x}} {{y}})", data_role="cell", stroke_width=1.6)


WORDS = [
    dict(number("StatusWordPanel", "自定义状态字面板", "Status word panel", "", (0, 65535), 165),
         mode="word", size=[330, 205], itemSvg=word_tile()),
    dict(number("DigitalTimingChart", "数字时序图", "Digital timing chart", "", (0, 65535), 165),
         mode="timing", size=[380, 235], itemSvg=timing_lane()),
]
MEASUREMENTS = [
    dict(number("RotaryEncoder", "旋转编码器", "Rotary encoder", "", (-(2**53-1), 2**53-1), 256),
         mode="encoder", size=[245, 160]),
    dict(number("DcPowerSupply", "直流电源面板", "DC power supply", "", (0, 1),
                {"voltage": 12.0, "current": .425, "mode": "CV"}),
         mode="supply", size=[255, 175]),
]
BATTERY = [
    dict(number("BmsCellMatrix", "电芯电压矩阵", "Battery cell voltages", "V", (0, 1),
                [3.692, 3.701, 3.687, 3.694, 3.698, 3.691, 3.695, 3.690], 3),
         mode="cells", size=[365, 220], itemSvg=battery_cell()),
]
VALVES = [
    dict(state("DirectionalValve", "三位四通换向阀", "4/3 directional valve",
               ["中位封闭", "P→A，B→T", "P→B，A→T"], ["Closed center", "P→A, B→T", "P→B, A→T"], 1),
         size=[245, 155]),
]


def signal_art(d, kind):
    d.art_size = (320, 160)
    body = rect(2, 2, 316, 156, "body", "edge", 5)
    if kind == "DigitalTimingChart":
        body += rect(5, 5, 310, 150, "surface", None, 4)
        body += "".join(line(f"M{x} 8v144", "muted", .5) for x in (82, 139, 196, 253, 310))
    return body + token("items")


def measurement_art(d, kind):
    if kind == "RotaryEncoder":
        disc = ""
        for angle in range(0, 360, 30):
            disc += transform(f"rotate({angle} 74 51)", rect(72, 21, 4, 7, "surface", "muted", 1))
        disc += line("M74 41V32", "accent", 3)
        pickup = (path("M113 39h22v24h-22v-5h-12V43h12Z")
                  + line("M135 45h12m-12 12h12", "edge", 1.8))
        return (ellipse(74, 51, 35, 35, "body", "edge") + ellipse(74, 51, 20, 20, "surface", "muted")
                + d.show("known", transform("rotate({{encoderAngle}} 74 51)", disc))
                + ellipse(74, 51, 7, 7, "body", "edge") + pickup
                + tag("g", line("M62 93h22l-5-4m5 4-5 4", "green", 2.6), opacity=token("forwardOn"))
                + tag("g", line("M85 93H63l5-4m-5 4 5 4", "warm", 2.6), opacity=token("reverseOn")))
    d.art_size = (220, 124)
    front = panel(8, 8, 204, 106)
    for y, value, unit in ((20, "voltageText", "V"), (62, "currentText", "A")):
        front += (rect(20, y, 127, 34, "surface", "edge", 3)
                  + text_at(79, y + 24, token(value), 20)
                  + text_at(132, y + 22, unit, 12, "muted"))
    for y, legend, driver, color in ((34, "CV", "cvOn", "green"), (77, "CC", "ccOn", "warm")):
        front += (ellipse(162, y, 4, 4, "surface", "muted") + label(183, y + 4, legend, 12)
                  + tag("g", ellipse(162, y, 4, 4, color, None), opacity=token(driver)))
    return front


def valve_art(d, _kind):
    d.art_size = (220, 100)
    art = line("M8 50h6l3-8 4 16 4-16 4 16 3-8h2m153 0h4l3-8 4 16 4-16 4 16 3-8h5", "muted", 1.5)
    for index, x in ((1, 32), (0, 84), (2, 136)):
        if index == 0:
            routes = "M13 3v12m-5 0h10m20-12v12m-5 0h10M13 43V31m-5 0h10m20 12V31m-5 0h10"
        elif index == 1:
            routes = "M13 41V6m-5 7 5-7 5 7M38 5v35m-5-7 5 7 5-7"
        else:
            routes = "M12 40L39 6m-8 3 8-3v8M12 6l27 34m-8-3 8 3v-8"
        active = (rect(1.5, 1.5, 49, 43, "surface", "accent", 2)
                  + line(routes, "accent", 2.5)
                  + label(13, -5, "A", 10) + label(38, -5, "B", 10)
                  + label(13, 59, "P", 10) + label(38, 59, "T", 10))
        art += transform(f"translate({x} 27)", rect(0, 0, 52, 46, "body", "edge", 1),
                         line(routes, "muted", 1.3), d.show(f"state{index}", active))
    return art


SECTIONS = (("embedded", WORDS, signal_art), ("diy", MEASUREMENTS, measurement_art),
            ("automotive", BATTERY, signal_art), ("equipment", VALVES, valve_art))
