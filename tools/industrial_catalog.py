#!/usr/bin/env python3
"""Build the industrial artwork embedded in Studio and optionally its Python and web peers.

From the Studio checkout, sync adjacent repositories with:
    python3 tools/industrial_catalog.py --python-root ../PyQt-UI-Designer --web-root ../modusignal
Add --check to verify the files without writing them. Each product ships its own
offline copy. A slot interpolates SVG coordinates using a normalized input.
"""

import argparse
import json
from pathlib import Path
from electronics_catalog import SECTIONS as ELECTRONICS_SECTIONS
from feedback_catalog import SECTIONS as FEEDBACK_SECTIONS
from industrial_demo import gallery_frames, gallery_project
from widget_art import arc_ticks, ellipse, label, line, number, panel, path, rect, state, tag, token, transform


INSTRUMENTS = [
    number("PressureGauge", "压力表", "Pressure gauge", "MPa", (0, 1.6), .64, 2),
    number("FlowMeter", "瞬时流量计", "Instantaneous flow meter", "m³/h", (-80, 80), 32.5, 1, "signed"),
    number("Thermometer", "温度计", "Thermometer", "°C", (-20, 150), 46),
    number("MagneticLevelGauge", "磁翻板液位计", "Magnetic level gauge", "mm", (0, 1500), 820),
    number("DifferentialPressureGauge", "差压表", "Differential pressure gauge", "kPa", (-20, 20), 3.2, 1),
    number("VacuumGauge", "真空表", "Vacuum gauge", "kPa", (-100, 0), -62),
    number("Rotameter", "转子流量计", "Rotameter", "L/min", (0, 25), 12, 1),
    number("FlowTotalizer", "累积流量计", "Flow totalizer", "m³", (0, 9999.9), 1536.7, 1),
    number("Tachometer", "转速表", "Tachometer", "rpm", (-3000, 3000), 1500),
    number("Ammeter", "电流表", "Ammeter", "A", (0, 100), 42),
    number("Voltmeter", "电压表", "Voltmeter", "V", (0, 500), 380),
    number("PowerMeter", "功率表", "Power meter", "kW", (-50, 50), 18.5, 1),
]
PUMPS_AND_VESSELS = [
    state("Pump", "离心水泵", "Centrifugal pump", ["停止","运行","故障"], ["Stopped","Running","Fault"], 1, "run"),
    state("OnOffValve", "开关阀", "On-off valve", ["关闭","全开","动作中"], ["Closed","Open","In transit"], 1),
    number("ControlValve", "调节阀", "Control valve", "%", (0, 100), 65),
    number("FlowPipe", "流向管道", "Flow pipe", "m³/h", (-100, 100), 45, 0, "signed"),
    number("Tank", "液位水箱", "Level tank", "%", (0, 100), 56),
    number("Silo", "粉料仓", "Bulk silo", "%", (0, 100), 68),
    number("WeighHopper", "称重料斗", "Weigh hopper", "kg", (0, 500), 175),
    number("Mixer", "搅拌罐", "Agitated tank", "rpm", (-120, 120), 60, 0, "signed"),
]
DRIVES_AND_AIR = [
    number("Motor", "电机", "Motor", "rpm", (-1800, 1800), 1450, 0, "signed"),
    number("Fan", "轴流风机", "Axial fan", "rpm", (0, 1800), 900, 0, "signed"),
    number("BeltConveyor", "带式输送机", "Belt conveyor", "m/s", (-2, 2), 1.2, 1, "signed"),
    number("ScrewConveyor", "螺旋输送机", "Screw conveyor", "rpm", (-120, 120), 65, 0, "signed"),
    state("AirCompressor", "空压机", "Air compressor", ["停机","加载","故障","卸载"], ["Stopped","Loaded","Fault","Unloaded"], 1, "compressor"),
    number("Cylinder", "气缸", "Pneumatic cylinder", "%", (0, 100), 65),
    number("Gripper", "气动夹爪", "Pneumatic gripper", "mm", (0, 80), 40),
    number("LinearAxis", "直线滑台", "Linear axis", "mm", (0, 500), 220),
]
HEAT_AND_POWER = [
    number("Heater", "电加热器", "Electric heater", "%", (0, 100), 65),
    number("HeatExchanger", "换热器", "Heat exchanger", "°C", (0, 120), 52),
    number("CoolingTower", "冷却塔", "Cooling tower", "rpm", (0, 900), 450, 0, "signed"),
    number("Boiler", "蒸汽锅炉", "Steam boiler", "MPa", (0, 1.6), .85, 2),
    state("CircuitBreaker", "断路器", "Circuit breaker", ["分闸","合闸","跳闸"], ["Open","Closed","Tripped"], 1),
    state("Contactor", "接触器", "Contactor", ["释放","吸合"], ["Released","Engaged"], 1),
    number("VFD", "变频器", "Variable-frequency drive", "Hz", (0, 50), 37.5, 1),
    state("StackLight", "三色灯塔", "Stack light",
          ["全灭","绿","黄","绿＋黄","红","绿＋红","黄＋红","红＋黄＋绿"],
          ["Off","Green","Amber","Green + amber","Red","Green + red","Amber + red","Red + amber + green"], 1, bits=True),
]

# Limits use the displayed engineering units. A disabled pair can be enabled in the property editor.
LIMITS = {"PressureGauge": ((.2, 1.2), False), "Tank": ((20, 80), True),
          "Voltmeter": ((340, 420), True), "Ammeter": ((0, 80), False)}

class Drawing:
    def __init__(self):
        self.slots = {}
        self.art_size = (180, 110)

    def slot(self, name, driver, start=0, end=1):
        spec = [driver, start, end]
        if name in self.slots and self.slots[name] != spec:
            raise ValueError(f"conflicting slot {name}")
        self.slots[name] = spec
        return token(name)

    def show(self, driver, *content):
        return tag("g", "".join(content), opacity=self.slot(driver, driver))

    def rotor(self, x, y, radius, count=5, squash=1):
        vane = path("M0 -3 Q8 -8 6 -19 Q21 -10 7 0Z", "muted", None)
        active = path("M0 -3 Q8 -8 6 -19 Q21 -10 7 0Z", "accent", None)
        blades = "".join(transform(f"rotate({n * 360 / count:g})", vane, self.show("active", active)) for n in range(count))
        spin = self.slot("spin", "phase", 0, 360)
        return transform(f"translate({x} {y}) scale({radius / 20 * squash:g} {radius / 20:g}) rotate({spin})",
                         blades, ellipse(0, 0, 3.5, 3.5, "ink", None))

    def column(self, x, y, height, color):
        top = self.slot("columnY", "level", y + height, y)
        fill = self.slot("columnH", "level", 0, height)
        ticks = "".join(line(f"M{x+10} {y+height*n/4:g}h{7 if n%2==0 else 4}") for n in range(5))
        return rect(x, y, 7, height, "body", "muted", 2) + self.show("known", rect(x+1, top, 5, fill, color, None)) + ticks


def feet(a, b, y):
    return "".join(path(f"M{x} {y}h8l3 9h-14Z", "body") + rect(x-4, y+9, 17, 3, "muted", "edge", 1) for x in (a,b))


def window(x, y, w, h, value, size=15):
    return rect(x,y,w,h,"surface","muted",2) + label(x+w/2,y+h/2+size*.34,value,size)


def fault(x,y):
    return transform(f"translate({x} {y})",path("M0 -8l9 15h-18Z","surface","danger",2),line("M0 -3v4m0 3v.5","danger",2))


def pointer(d, x, y, r, start, end, driver="level"):
    angle = d.slot("angle",driver,start,end)
    return d.show("known", transform(f"translate({x} {y}) rotate({angle})",
                                     path(f"M-4 -2L{r} 0L-4 2Z","accent",None)), ellipse(x,y,2.7,2.7,"ink",None))


def limit_ticks(d, x, y, r, start, end):
    ticks = []
    for driver,color in (("lowMark","warm"),("highMark","danger")):
        angle = d.slot(driver+"Angle",driver,start,end)
        ticks.append(transform(f"translate({x} {y}) rotate({angle})",line(f"M{r-6} 0h6",color,2.7)))
    return d.show("limitsOn",*ticks)


def meter_dial(d, kind):
    low = "0" if kind == "Tachometer" else token("minLabel")
    face = ellipse(90,47,39,39,"body") + ellipse(90,47,36.5,36.5,"surface")
    marks = arc_ticks(90,47,34,140,400,16) + label(90,62,token("unit"),10)
    marks += label(73,72,low,9) + label(107,72,token("maxLabel"),9)
    if kind == "DifferentialPressureGauge":
        marks += label(90,29,"0",10)
    dial = face + marks + pointer(d,90,47,27,140,400,"magnitude" if kind=="Tachometer" else "level")
    if kind == "Tachometer":
        return path("M64 74L52 91h76l-13-17Z") + dial
    return transform("translate(0 8)", dial)


def flow_arrows(d,x,y,spacing=25):
    phase = d.slot(f"travel{spacing}","phase",0,spacing)
    right = path("M-5 -3h5l5 3l-5 3h-5l4-3Z","accent",None)
    marks = "".join(transform(f"translate({x+n*spacing} {y})", d.show("positive",right),
                              d.show("negative",transform("rotate(180)",right))) for n in range(4))
    return transform(f"translate({phase} 0)",marks)


def instruments(d, kind):
    if kind == "PressureGauge":
        digits = label(90,58,token("reading"),token("readingSize"))
        return (panel(16,16,148,78) + rect(24,24,132,62,"surface","muted",4)
                + tag("g",digits,font_weight="bold") + label(90,76,token("unit"),12)
                + d.show("lowAlarm",rect(29,33,3,44,"warm",None,1.5))
                + d.show("highAlarm",rect(29,33,3,44,"danger",None,1.5)))
    if kind in ("DifferentialPressureGauge","VacuumGauge","Tachometer"):
        return meter_dial(d,kind)
    if kind == "FlowMeter":
        return (rect(48,68,84,22,"body","edge",6) + rect(52,72,76,14,"surface","muted",3)
                + rect(85,51,10,17) + panel(51,12,78,39)
                + window(57,19,66,23,token("reading")) + flow_arrows(d,62,79,14))
    if kind == "Thermometer":
        return (rect(67,10,44,84,"body","edge",6) + d.column(80,17,60,"warm")
                + ellipse(83.5,82,8,8,"body") + d.show("known",ellipse(83.5,82,7,7,"warm",None))
                + rect(80,94,8,10) + label(130,24,token("maxLabel")) + label(130,80,token("minLabel")))
    if kind == "MagneticLevelGauge":
        body = rect(66,9,14,90,"body","edge",5) + rect(84,12,24,86,"body","edge",3)
        for n in range(12):
            y = 89-n*6.7
            body += rect(87,y,18,5.4,"surface",None,1) + d.show("seg"+str(n+1),rect(87,y,18,5.4,"danger",None,1))
        return body+label(137,21,token("maxLabel"),11)+label(136,96,token("minLabel"),11)
    if kind == "Rotameter":
        y = d.slot("floatY","level",85,23)
        return (path("M68 15h35l-8 79H79Z","surface")
                + "".join(line(f"M104 {20+n*12}h{10 if n%2==0 else 5}") for n in range(7))
                + d.show("known",transform(f"translate(86 {y})",path("M-7 -3H7L3 4h-6Z","accent"),line("M-9 -3H9","ink",1)))
                + label(132,25,token("maxLabel")) + label(132,92,token("minLabel")))
    if kind == "FlowTotalizer":
        return (panel(27,28,126,54) + window(35,37,110,29,token("reading"),19) + line("M41 73h98"))
    if kind == "Ammeter":
        return (panel(32,18,116,73)+rect(39,24,102,56,"surface","muted",3)+arc_ticks(90,70,38,210,330,12)
                + label(90,53,token("unit"))+label(55,69,token("minLabel"),10)+label(125,69,token("maxLabel"),10)
                + limit_ticks(d,90,70,38,210,330)+pointer(d,90,70,31,210,330)+rect(46,91,10,9)+rect(123,91,10,9))
    if kind == "Voltmeter":
        return (panel(31,22,118,62)+window(39,30,102,29,token("reading")+" "+token("unit"),18)
                + line("M47 70h43m-43 4h43")+ellipse(127,70,3,3,"body")+d.show("known",ellipse(127,70,2,2,"accent",None))
                + d.show("lowAlarm",ellipse(127,70,3,3,"warm",None))+d.show("highAlarm",ellipse(127,70,3,3,"danger",None))
                + rect(48,85,10,10)+rect(123,85,10,10))
    if kind == "PowerMeter":
        extent = d.slot("barLength","magnitude",0,48)
        left = d.slot("barLeft","magnitude",90,42)
        return (panel(26,19,128,72)+window(40,27,100,26,token("reading")+" "+token("unit"),15)
                + rect(41,65,98,7,"muted",None,2)+d.show("positive",rect(90,65,extent,7,"accent",None))
                + d.show("negative",rect(left,65,extent,7,"accent",None))+line("M90 60v17","edge",1.5)
                + label(44,84,"−")+label(136,84,"+"))
    raise KeyError(kind)


def pumps_and_vessels(d, kind):
    if kind == "Pump":
        casing = "M53 61C53 36 73 18 98 18Q119 18 130 32L119 40C131 57 128 80 112 89C85 104 53 88 53 61Z"
        return (feet(65,100,86) + path(casing) + ellipse(88,62,27,27) + ellipse(88,62,22,22,"surface","muted")
                + d.rotor(88,62,20) + d.show("state2",fault(139,72)))
    if kind == "OnOffValve":
        seat = ellipse(90,75,19,19,"surface","edge")
        disc = ellipse(0,0,15,2.7,"accent","edge")
        for state_index, angle in ((0,90),(1,0),(2,40)):
            seat += d.show(f"state{state_index}",transform(f"translate(90 75) rotate({angle})",disc))
        return (ellipse(90,75,23,23)+rect(86,40,8,13)+panel(65,17,50,23)
                + line("M75 28h30")+seat)
    if kind == "ControlValve":
        y = d.slot("stemY","level",84,64)
        return (path("M58 74C69 71 72 65 75 60H105Q111 72 120 75L114 87Q90 102 67 87Z")
                + path("M59 32Q65 12 90 12Q115 12 121 32Z")+rect(58,32,64,5)
                + line("M72 39l8 21h20l8-21","edge",2)+line("M75 86h10m10 0h10","edge",2)
                + d.show("known",line(f"M90 38V{y}","edge",2.7),transform(f"translate(90 {y})",path("M-7 -3h14l-3 5h-8Z","accent"))))
    if kind == "FlowPipe":
        elbow = "M24 83h80q23 0 23-23V22"
        size = d.slot("pipeSize","thickness",8,26)
        outline = d.slot("pipeOutline","thickness",11,29)
        travel = d.slot("verticalTravel","phase",0,-16)
        up = path("M0 -5l4 5v5L0 1l-4 4V0Z","accent",None)
        arrows = transform("translate(0 "+travel+")",d.show("positive",transform("translate(127 55)",up)),
                           d.show("negative",transform("translate(127 55) rotate(180)",up)))
        curved = (tag("g",line(elbow,"edge",outline)+line(elbow,"body",size),stroke_linecap="butt")
                  + flow_arrows(d,31,83,17)+arrows)
        straight_path = "M24 56h132"
        straight = (tag("g",line(straight_path,"edge",outline)+line(straight_path,"body",size),stroke_linecap="butt")
                    + flow_arrows(d,33,56,27))
        offset = d.slot("mirrorOffset","mirrored",0,180)
        scale_x = d.slot("mirrorScale","mirrored",1,-1)
        return transform(f"translate({offset} 0) scale({scale_x} 1)",d.show("elbow",curved),d.show("straight",straight))
    if kind == "Tank":
        top = d.slot("liquidTop","level",90,18)
        back = d.slot("liquidBack","meniscus",9,-9)
        shell = "M44 18a46 8 0 0 1 92 0v72a46 8 0 0 1-92 0Z"
        # The ellipse opens over the first 9 of the tank's 72 vertical units.
        # At the minimum both curves coincide with the base, leaving zero filled area.
        rear_arc = f"c0 {back} -88 {back} -88 0Z"
        water = f"M46 {top}V90c0 9 88 9 88 0V{top}" + rear_arc
        surface = f"M46 {top}c0 9 88 9 88 0" + rear_arc
        lo, hi = d.slot("lowY","lowMark",90,18), d.slot("highY","highMark",90,18)
        return (path(shell,"surface") + d.show("known",path(water,"liquid",None),
                tag("g",path(surface,"surface",None),opacity=.38)) + path(shell,None)
                + ellipse(90,18,46,8,None,"edge")
                + d.show("limitsOn",line(f"M122 {lo}h9","warm",2),line(f"M122 {hi}h9","danger",2)))
    if kind == "Silo":
        outline = "M51 31l18-12h42l18 12v34l-32 26v8H83v-8L51 65Z"
        body = line("M56 65v35h10m58-35v35h-10","edge",2.6) + path(outline)
        def half_width(y):
            return min(37, 6 + max(0, 91 - y) * 1.18)
        for n in range(1,13):
            bottom, top = 97-(n-1)*5.3, 97-n*5.3
            low, high = half_width(bottom), half_width(top)
            shape = path(f"M{90-low:.1f} {bottom:.1f}h{2*low:.1f}L{90+high:.1f} {top:.1f}H{90-high:.1f}Z","grain",None)
            dots = "".join(ellipse(90+j*8+(n%2)*2,top+2,.8,.8,"warm",None)
                           for j in range(-4,5) if abs(j*8+(n%2)*2)<high-2)
            body += d.show(f"seg{n}",shape,dots)
        return body + path(outline,None) + line("M51 31h78") + rect(80,99,20,4)
    if kind == "WeighHopper":
        supports = line("M25 54h31m61 0h22M31 55v36m102-36v36","edge",2.5)
        for x in (42,116):
            supports += rect(x,44,13,9)+line(f"M{x+2} 51l3-5 3 5 3-5","edge",1.1)
        return (supports + path("M46 27h76v19L92 79v12H77V79L46 46Z")+rect(43,25,82,4)
                + rect(74,91,20,4)+line("M51 40h66")+d.column(146,30,58,"accent"))
    if kind == "Mixer":
        return (feet(63,101,86)+path("M53 42Q53 32 89 32Q125 32 125 42V73Q125 91 105 91H73Q53 91 53 73Z")
                + ellipse(89,42,36,6,None,"muted")+rect(82,22,14,12)+panel(73,9,32,16)+line("M89 25v56","edge",2.5)
                + transform("translate(89 60) scale(1 .28)",d.rotor(0,0,24,3))
                + transform("translate(89 78) scale(1 .28)",d.rotor(0,0,21,3)))
    raise KeyError(kind)


def drives_and_air(d, kind):
    if kind == "Motor":
        return (feet(59,105,81)+rect(29,54,27,10)+rect(28,51,8,16)
                + path("M51 37Q60 29 72 29h36q18 0 27 15v29q-7 13-25 13H72Q56 86 50 73Z")
                + rect(70,19,29,12,"body","edge",2)
                + "".join(line(f"M{63+abs(y-58)//12} {y}h42","muted",1.8) for y in range(38,81,7))
                + ellipse(123,58,17,25)+ellipse(123,58,12,19,"surface","muted")+d.rotor(123,58,16,5,.62))
    if kind == "Fan":
        return (panel(45,9,90,90)+ellipse(90,54,39,39)+ellipse(90,54,35,35,"surface","muted")
                + d.rotor(90,54,33,6)+ellipse(90,54,26,26,None,"muted")+ellipse(90,54,15,15,None,"muted")
                + line("M63 26l54 56m-54 0l54-56","muted",.8))
    if kind == "BeltConveyor":
        phase = d.slot("band","phase",0,12)
        belt = rect(25,44,130,32,"body","edge",16)+rect(30,49,120,22,"surface","muted",11)
        for x in (43,137):
            belt += ellipse(x,60,9,9)+d.rotor(x,60,7,4)
        stripes = "".join(line(f"M{43+12*n} 46l-4 5","muted",2) for n in range(8))
        return line("M49 74l-7 21h16m71-21 8 21h-15","edge",3)+belt+transform(f"translate({phase} 0)",stripes)
    if kind == "ScrewConveyor":
        chamber = rect(27,49,126,29,"body","edge",5)+rect(33,54,114,19,"surface","muted",3)
        band = d.slot("pitch","phase",0,20)
        screw = path("M23 64"+"q5 -17 10 0t10 0"*7,None,"muted",2.3)
        active = path("M23 64"+"q5 -17 10 0t10 0"*7,None,"accent",2.3)
        return (feet(55,116,79)+path("M37 23h28l-6 16v10H44V39Z")+path("M115 78h25v10h-7v12h-11V88Z")
                + chamber+line("M30 64h120","edge",2)+transform(f"translate({band} 0)",screw,d.show("active",active))
                + rect(15,49,24,29,"surface",None)+rect(143,49,37,29,"surface",None)
                + rect(27,49,10,28,"body","edge",2)+rect(144,49,8,28,"body","edge",2)+rect(18,58,9,11))
    if kind == "AirCompressor":
        return (feet(55,107,86)+rect(38,62,104,27,"body","edge",13)+rect(51,57,77,5)
                + path("M66 54L52 35l11-7 16 19Zm15-8 10-23 12 7-11 23Z")
                + line("M51 32l14-9m-11 14 14-9m22-7 16 8m-18-3 16 8","edge",2)
                + ellipse(81,51,11,11,"surface")+d.rotor(81,51,9,4)+rect(104,40,22,15,"body","edge",2)
                + line("M107 45h15m-15 5h15")+rect(102,72,23,6,"surface","muted",3)
                + d.show("state1",rect(103,73,21,4,"accent",None,2))+d.show("state2",fault(147,45)))
    if kind == "Cylinder":
        pos = d.slot("stroke","level",0,50)
        piston = transform(f"translate({pos} 0)",rect(43,55,69,8),rect(39,45,7,26,"accent"),rect(109,52,5,14))
        return (rect(26,39,84,38,"body","edge",3)+rect(32,44,72,28,"surface","muted",1)
                + rect(23,35,7,47)+rect(105,35,7,47)
                + d.show("pistonKnown",piston)+line("M27 82v9h13m68-9v9h-11","edge",2))
    if kind == "Gripper":
        fixed = rect(72,14,36,12,"body","edge",2)+panel(55,25,70,32)+line("M64 46h52")
        for sign, side in ((-1,"left"),(1,"right")):
            x = d.slot(side,"level",90+5*sign,90+32*sign)
            finger = path("M-4 0h9v29l-5 7h-7v-5h4Z") + line("M-3 26v6h-4","accent",2)
            fixed += d.show("known",transform(f"translate({x} 57) scale({sign} 1)",finger))
        return fixed
    if kind == "LinearAxis":
        x = d.slot("carriage","level",34,119)
        base = feet(43,121,76)+rect(22,60,136,13,"body","edge",2)+rect(22,73,136,4)+rect(25,47,9,13)+rect(147,47,9,13)
        ruler = "".join(line(f"M{31+9.5*n:g} 62v{6 if n%3==0 else 3}") for n in range(13))
        return base+ruler+d.show("known",transform(f"translate({x} 0)",rect(0,40,24,19,"body","edge",2),
                                 rect(-3,36,30,5,"accent"),ellipse(5,50,2,2,"surface"),ellipse(18,50,2,2,"surface")))
    raise KeyError(kind)


def heat_and_power(d, kind):
    if kind == "Heater":
        coil = "M51 43h12V33q0-7 7-7t7 7v39q0 6 6 6t6-6V33q0-7 7-7t7 7v39q0 6 6 6t6-6V33q0-7 7-7t7 7v47q0 6-6 6H59V73h-8"
        return (rect(30,37,21,12,"body","edge",2)+rect(30,67,21,12,"body","edge",2)
                + line("M22 43h8m-8 30h8","edge",2.3)+line(coil,"edge",5)+line(coil,"body",2)
                + tag("g",line(coil,"warm",3.2),opacity=d.slot("heat","level")))
    if kind == "HeatExchanger":
        return (feet(52,100,82)+path("M48 27l12-9h59v59l-12 8H48Z")+rect(48,28,59,57,"body","edge",2)
                + "".join(line(f"M{54+7*n} 32v46","muted",1.8) for n in range(7))
                + line("M58 35h40m-40 37h40","edge",2)+d.column(127,26,57,"warm"))
    if kind == "CoolingTower":
        outer = path("M63 24h54q-8 21 0 34l16 28H47l16-28q8-17 0-34Z")
        slats = "".join(line(f"M{90-w} {y}h{2*w}","muted",2) for y,w in ((59,23),(65,26),(71,29),(77,32)))
        return (outer+ellipse(90,24,27,8)+ellipse(90,24,21,5.5,"surface","muted")
                + transform("translate(90 24) scale(1 .26)",d.rotor(0,0,20))
                + slats+line("M57 86v7m66-7v7","edge",3)+path("M42 93h96l-4 7H46Z"))
    if kind == "Boiler":
        return (feet(52,105,86)+rect(32,47,105,42,"body","edge",19)+ellipse(49,68,13,18)
                + ellipse(49,68,7,11,"surface","muted")+line("M68 54h46m-46 27h43")
                + rect(110,37,13,10)+ellipse(116,25,15,15,"surface")
                + arc_ticks(116,25,12,145,395,6)+pointer(d,116,25,9,145,395))
    if kind == "CircuitBreaker":
        shell = panel(53,16,74,76)+line("M66 24h48m-48 59h48")
        for x in (69,103):
            shell += rect(x,9,8,7)+rect(x,92,8,8)
        contacts = line("M73 32v33m33-33v8","edge",2.5)+ellipse(73,65,3,3,"edge")+ellipse(106,41,3,3,"edge")
        return (shell+contacts+d.show("state0",line("M73 65l5-33","edge",3))
                +d.show("state1",line("M73 65l33-24","accent",3))+d.show("state2",line("M73 65l18-30","edge",3),fault(108,68)))
    if kind == "Contactor":
        shell = panel(38,19,104,73)
        bridge = ""
        for x in (59,90,121):
            shell += (rect(x-4,12,8,7)+rect(x-4,92,8,8)+line(f"M{x-5} 31v14m10 0V31","edge",1.8)
                      +ellipse(x-5,45,1.7,1.7,"edge")+ellipse(x+5,45,1.7,1.7,"edge"))
            bridge += line(f"M{x-7} 47h14","edge",3)+line(f"M{x} 47v9")
        moving = bridge + line("M58 56h64","edge",2)
        return (shell+d.show("state0",transform("translate(0 10)",moving))+d.show("state1",moving)
                + rect(76,74,28,12,"body","edge",2)+line("M80 84l5-8m2 8 5-8m2 8 5-8"))
    if kind == "VFD":
        width = d.slot("frequencyWidth","level",0,40)
        return (panel(50,8,80,93)+window(58,18,64,22,token("reading"))+label(114,51,token("unit"),10)
                + rect(60,46,40,5,"muted",None,2)+d.show("known",rect(60,46,width,5,"accent",None,2))
                + line("M58 61h62")+"".join(line(f"M{62+7*n} 68v22","muted",2) for n in range(9))
                + "".join(rect(x,101,4,5) for x in (60,78,96,114)))
    if kind == "StackLight":
        body = rect(85,76,10,18)+rect(75,94,30,7,"body","edge",2)+rect(70,10,40,7,"body","edge",3)
        for n, (bit,color) in enumerate(((2,"danger"),(1,"yellow"),(0,"green"))):
            y = 17+n*20
            lens = rect(71,y,38,19,color,"edge",3) + rect(77,y+4,3,9,"surface",None,1)
            shine = line(f"M63 {y+9}h-6m60 0h6",color,1.8)
            body += rect(71,y,38,19,"body","edge",3)+line(f"M78 {y+8}h24")+d.show(f"bit{bit}",lens,shine)
        return body+rect(70,76,40,4,"body","edge",1)
    raise KeyError(kind)


def catalog():
    rows = []
    sections = (("instrument", INSTRUMENTS, instruments),
                ("equipment", PUMPS_AND_VESSELS, pumps_and_vessels),
                ("equipment", DRIVES_AND_AIR, drives_and_air),
                ("equipment", HEAT_AND_POWER, heat_and_power), *ELECTRONICS_SECTIONS, *FEEDBACK_SECTIONS)
    for group, entries, draw in sections:
        for entry in entries:
            drawing = Drawing()
            art = draw(drawing, entry["type"])
            width, height = drawing.art_size
            svg = tag("svg",art,xmlns="http://www.w3.org/2000/svg",width=width,height=height,
                      viewBox=f"0 0 {width} {height}",stroke_linecap="round",stroke_linejoin="round")
            row = dict(entry,group=group,slots=drawing.slots,svg=svg)
            if entry["type"] in LIMITS:
                row["limits"], row["limitsOn"] = LIMITS[entry["type"]]
            rows.append(row)
    return rows


def type_tables(rows, groups):
    sections = []
    for group, title in groups:
        lines = [f"## {title}", "", "| 类型 | 默认标题 | 单位 | 量程 | 示意值 | 小数位 | 状态码 |",
                 "| --- | --- | --- | --- | --- | --- | --- |"]
        for entry in rows:
            if entry["group"] != group:
                continue
            states = "，".join(f"{n} {label}" for n,label in enumerate(entry["states"]))
            if entry["mode"] == "flags":
                bits = [f"位 {n} {label}" for n, label in enumerate(entry["flags"])]
                states = "，".join([f"0 {entry['clearLabel']}", *bits])
            span = {"word": "8 或 16 位", "timing": "8 或 16 位", "encoder": "带符号整数",
                    "cells": "电压数组", "supply": "电源反馈对象"}.get(entry["mode"],
                                                              f"{entry['minimum']}…{entry['maximum']}")
            sample = {"cells": "[3.692, 3.701, …]", "supply": "12.00 V、0.425 A、CV"}.get(entry["mode"], entry["value"])
            decimals = {"supply": "电压 2、电流 3", "encoder": "计数 0、圈数 3"}.get(entry["mode"], entry["decimals"])
            lines.append(f"| `{entry['type']}` | {entry['name']} | {entry['unit'] or '—'} | "
                         f"{span} | {sample} | {decimals} | {states or '—'} |")
        sections.append("\n".join(lines))
    return "\n\n".join(sections) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--python-root", type=Path, help="also update this PyQt-UI-Designer checkout")
    parser.add_argument("--web-root", type=Path, help="also update this modusignal checkout")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    rows = catalog()
    document = json.dumps(rows, ensure_ascii=False, indent=2) + "\n"
    demo = {
        "动态控件全景.uidp": json.dumps(gallery_project(rows), ensure_ascii=False, indent=2) + "\n",
        "动态反馈.jsonl": gallery_frames(rows),
    }
    outputs = {root / "resources/industrial/catalog.json": document}
    for target_root in (root, args.python_root, args.web_root):
        if target_root:
            for name, content in demo.items():
                outputs[target_root / "examples/industrial" / name] = content
    guides = {
        "industrial.md": (("instrument", "工业仪表"), ("equipment", "工业设备")),
        "electronics.md": (("automotive", "汽车电子"), ("diy", "电子 DIY"), ("embedded", "嵌入式")),
    }
    marker = "<!-- 以下类型表由 tools/industrial_catalog.py 生成 -->"
    for name, groups in guides.items():
        guide = root / "docs/uidp" / name
        intro, found, _ = guide.read_text(encoding="utf-8").partition(marker)
        if not found:
            parser.error(f"Missing table marker in {guide}")
        outputs[guide] = intro + marker + "\n\n" + type_tables(rows, groups)
    if args.python_root:
        outputs[args.python_root / "app/core/industrial_catalog.json"] = document
    if args.web_root:
        outputs[args.web_root / "src/hmi/industrial-catalog.js"] = "export default " + document.rstrip() + ";\n"
    for peer in (args.python_root, args.web_root):
        if peer:
            for name in ("恒压供水泵站.uidp", "工业控件回放.uidp", "反馈回放.jsonl",
                         "电子控件回放.uidp", "电子反馈.jsonl", "replay.py"):
                relative = Path("examples/industrial") / name
                outputs[peer / relative] = (root / relative).read_text(encoding="utf-8")
    for target, content in outputs.items():
        if target.is_file() and target.read_text(encoding="utf-8") == content:
            continue
        if args.check:
            parser.exit(1, f"Outdated industrial catalogue: {target}\n")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
