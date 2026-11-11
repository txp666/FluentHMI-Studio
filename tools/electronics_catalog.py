"""Automotive, electronics DIY and embedded symbols for the shared device catalog."""

from widget_art import arc_ticks, ellipse, flags, label, line, number, paint, panel, path, rect, state, tag, token, transform


AUTOMOTIVE = [
    number("VehicleSpeed", "车速", "Vehicle speed", "km/h", (0, 240), 64),
    state("GearPosition", "挡位显示", "Gear position", list("PRNDSL"), list("PRNDSL"), 3),
    number("SteeringAngle", "方向盘转角", "Steering angle", "°", (-450, 450), -35),
    number("TractionBattery", "动力电池电量", "Traction battery charge", "%", (0, 100), 72),
    state("TurnIndicators", "转向灯", "Turn indicators", ["熄灭", "左转", "右转", "双闪"],
          ["Off", "Left", "Right", "Hazard"], 1),
    number("PedalPosition", "踏板开度", "Pedal position", "%", (0, 100), 35),
    flags("DoorStatus", "四门状态", "Door status", ["左前门", "右前门", "左后门", "右后门"],
          ["Front left", "Front right", "Rear left", "Rear right"], "全部关闭", "All closed", 9),
    number("TirePressure", "胎压", "Tire pressure", "kPa", (0, 400), 235),
    number("FuelLevel", "燃油余量", "Fuel level", "%", (0, 100), 63),
    number("CoolantTemperature", "冷却液温度", "Coolant temperature", "°C", (-40, 140), 88),
    state("HeadlightMode", "前照灯模式", "Headlight mode", ["关闭", "近光", "远光"],
          ["Off", "Low beam", "High beam"], 1),
    state("ChargeState", "车辆充电状态", "Vehicle charging status", ["未连接", "已连接", "充电中", "已充满", "充电故障"],
          ["Disconnected", "Connected", "Charging", "Fully charged", "Charging fault"], 2),
]
DIY = [
    number("ServoPosition", "舵机角度", "Servo angle", "°", (0, 180), 65),
    number("Potentiometer", "电位器位置", "Potentiometer position", "%", (0, 100), 42),
    state("RelayModule", "继电器模块", "Relay module", ["释放", "吸合"], ["Released", "Engaged"], 1),
    state("RgbLed", "三色 LED", "RGB LED", ["熄灭", "红", "绿", "黄", "蓝", "品红", "青", "白"],
          ["Off", "Red", "Green", "Yellow", "Blue", "Magenta", "Cyan", "White"], 6, bits=True),
    number("UltrasonicDistance", "超声测距", "Ultrasonic distance", "cm", (2, 400), 38.5, 1),
    number("Illuminance", "光照传感器", "Illuminance sensor", "lx", (0, 1000), 420),
    dict(number("SevenSegmentDisplay", "七段数码管", "Seven-segment display", "", (0, 255), 109), mode="byte"),
    state("Buzzer", "有源蜂鸣器", "Active buzzer", ["静音", "鸣响"], ["Silent", "Sounding"], 1),
    state("TouchPad", "触摸按键", "Capacitive touch pad", ["未触摸", "已触摸"], ["Untouched", "Touched"], 1),
    number("AirHumidity", "空气湿度", "Relative humidity", "%RH", (0, 100), 56),
    number("LoadCellScale", "电子秤", "Weighing scale", "g", (0, 5000), 128.4, 1),
    state("IrBeam", "对射光电门", "Infrared break beam", ["光束畅通", "光束遮挡"], ["Clear", "Blocked"], 1),
]
EMBEDDED = [
    dict(number("GpioBank", "GPIO 端口", "GPIO port", "", (0, 255), 165), mode="byte"),
    number("AdcChannel", "ADC 采样值", "ADC reading", "", (0, 4095), 1536),
    number("PwmDuty", "PWM 占空比", "PWM duty cycle", "%", (0, 100), 37.5, 1),
    number("CpuLoad", "CPU 负载", "CPU load", "%", (0, 100), 34),
    number("MemoryUsage", "内存占用", "Memory usage", "%", (0, 100), 62),
    state("BusStatus", "总线状态", "Bus status", ["离线", "在线", "错误"], ["Offline", "Online", "Error"], 1),
    state("ResetCause", "复位原因", "Reset cause", ["上电复位", "外部复位", "软件复位", "看门狗复位", "欠压复位"],
          ["Power-on reset", "External reset", "Software reset", "Watchdog reset", "Brownout reset"], 3),
    number("UartRxBuffer", "串口接收缓冲区", "UART RX buffer", "B", (0, 256), 96),
    flags("UartErrors", "UART 错误标志", "UART error flags", ["校验错误", "帧错误", "接收溢出", "噪声"],
          ["Parity error", "Framing error", "Overrun", "Noise"], "无错误", "No errors", 5),
    number("WirelessRssi", "无线信号强度", "Wireless signal strength", "dBm", (-100, -30), -67),
    state("RtosTaskState", "RTOS 任务状态", "RTOS task state", ["就绪", "运行", "阻塞", "挂起"],
          ["Ready", "Running", "Blocked", "Suspended"], 1),
    dict(number("I2cAddress", "I²C 7 位地址", "7-bit I²C address", "", (0, 127), 60), mode="byte"),
]


def automotive(d, kind):
    if kind == "VehicleSpeed":
        angle = d.slot("speedAngle", "level", -180, 0)
        car = (path("M66 69l6-13q2-4 7-4h22q5 0 7 4l6 13v11H66Z", "surface")
               + line("M72 66h36m-36 8h5m26 0h5", "edge", 2) + rect(68,80,7,4) + rect(105,80,7,4))
        marker = transform(f"rotate({angle} 90 73)", ellipse(146,73,4,4,"accent",None))
        return (line("M34 73a56 56 0 0 1 112 0","muted",4)
                + arc_ticks(90,73,50,180,360,12) + car + d.show("known",marker))
    if kind == "GearPosition":
        body = rect(16,33,148,45,"body","edge",8)
        for i, gear in enumerate("PRNDSL"):
            x = 20 + 24*i
            body += (rect(x,38,20,35,"surface",None,4)
                     + d.show(f"state{i}",rect(x,38,20,35,"body","accent",4),rect(x+3,66,14,3,"accent",None,1))
                     + label(x+10,60,gear,15))
        return body
    if kind == "SteeringAngle":
        spokes = (path("M82 50L56 40l-2 7 27 13 5 27h8l5-27 27-13-2-7-26 10Z")
                  + ellipse(90,55,12,10,"body","edge") + line("M87 20h6","accent",4))
        return (ellipse(90,55,38,38,"body") + ellipse(90,55,31,31,"surface")
                + d.show("known",transform(f"rotate({token('degrees')} 90 55)",spokes)))
    if kind == "TractionBattery":
        charge = d.slot("chargeWidth","level",0,106)
        cells = "".join(line(f"M{x} 37v35","muted",1) for x in range(54,141,21))
        return (rect(146,44,8,21,"muted","edge",2) + rect(26,28,120,54,"body","edge",6)
                + rect(33,35,106,40,"surface","muted",3)
                + d.show("known",rect(33,36,charge,38,"green",None,2)) + cells)
    if kind == "TurnIndicators":
        arrow = "M38 55l19-17v10h16v14H57v10Z"
        pair = rect(23,29,134,52,"body","edge",9)
        for driver, orientation in (("bit0","translate(0 0)"),("bit1","translate(180 0) scale(-1 1)")):
            pair += transform(orientation,path(arrow,"surface","muted"),
                              d.show(driver,path(arrow,"green","edge")))
        return pair
    if kind == "PedalPosition":
        tilt = d.slot("pedalAngle","level",-17,22)
        pedal = (line("M90 34v22l14 16","edge",5) + rect(94,62,23,33,"body","edge",5)
                 + line("M99 69h13m-13 5h13m-13 5h13m-13 5h13","muted",1.6))
        return (path("M66 18h50v8H99l-4 11H82l-3-11H66Z") + arc_ticks(90,34,60,54,113,6)
                + d.show("known",transform(f"rotate({tilt} 90 34)",pedal)) + ellipse(90,34,5,5,"surface"))
    if kind == "DoorStatus":
        chassis = (path("M63 32q0-21 15-23q12-2 24 0q15 2 15 23v49q0 17-14 19H77q-14-2-14-19Z")
                   + path("M69 32q21-8 42 0l-5 13H74Z", "surface", "muted")
                   + path("M74 70h32l5 13q-21 6-42 0Z", "surface", "muted")
                   + line("M67 35v39m46-39v39M73 48v19m34-19v19", "muted", 1.3)
                   + line("M69 17l7-2m28 0 7 2", "edge", 2.5))
        for bit, (side, y) in enumerate(((1, 35), (-1, 35), (1, 59), (-1, 59))):
            hinge = 63 if side == 1 else 117
            door = path("M0 0l-17 10-5 13 8-2L0 7Z", "warm", "edge")
            chassis += d.show(f"bit{bit}", transform(f"translate({hinge} {y}) scale({side} 1)", door))
        return chassis
    if kind == "TirePressure":
        wheel = (ellipse(81, 55, 35, 38, "body", "edge") + ellipse(75, 55, 31, 38, "body", "edge")
                 + ellipse(75, 55, 21, 27, "surface", "edge") + ellipse(75, 55, 15, 21, "body", "muted"))
        for y in (29, 40, 52, 64, 76):
            x = 109 if y in (29, 76) else 113
            wheel += line(f"M{x-6} {y+5}l6-4", "muted", 1.6)
        spokes = "".join(transform(f"rotate({a} 75 55)", line("M75 49V36", "edge", 2))
                         for a in range(0, 360, 72))
        return wheel + spokes + ellipse(75, 55, 5, 5, "surface", "edge") + d.column(129, 29, 52, "accent")
    if kind == "FuelLevel":
        dispenser = (rect(30, 25, 39, 58, "body", "edge", 4) + rect(36, 31, 27, 17, "surface", "edge", 2)
                     + rect(26, 82, 47, 5, "body", "edge", 1)
                     + line("M69 49q9 0 9 8v12q0 8 6 8t6-8V45l-8-8", "edge", 3)
                     + path("M81 35l8 5 3 7-5 1-5-6Z", "muted", "edge"))
        width = d.slot("fuelWidth", "level", 0, 51)
        gauge = (rect(101, 43, 55, 20, "surface", "edge", 2)
                 + d.show("known", rect(103, 45, width, 16, "green", None, 1))
                 + line("M115 44v6m14-6v9m13-9v6", "edge", 1)
                 + label(104, 76, "E", 11) + label(154, 76, "F", 11))
        return dispenser + gauge
    if kind == "CoolantTemperature":
        waves = (line("M79 70q7-6 14 0t14 0t14 0t14 0t14 0", "liquid", 2.6)
                 + line("M79 80q7-6 14 0t14 0t14 0t14 0t14 0", "liquid", 2.6))
        top = d.slot("mercuryTop", "level", 72, 24)
        return (path("M51 65V26a7 7 0 0 1 14 0v39a13 13 0 1 1-14 0Z", "surface", "edge")
                + d.show("known", line(f"M58 74V{top}", "warm", 5), ellipse(58, 76, 8, 8, "warm", None))
                + line("M69 26h11m-11 8h6m-6 8h11m-11 8h6m-6 8h11", "edge", 1.5) + waves)
    if kind == "HeadlightMode":
        lens = "M94 28q38 0 47 25q2 9-7 14-15 9-39 9-10-20-1-48Z"
        low = line("M78 38H61L40 49m38 2H61L40 62m38 2H61L40 75", "green", 3)
        high = line("M78 38H37m41 13H32m46 13H37", "accent", 3)
        return (path(lens, "body", "edge") + line("M100 34q-6 19 0 35", "muted", 1.6)
                + d.show("state1", line("M94 32q-8 19 0 39", "green", 3), low)
                + d.show("state2", line("M94 32q-8 19 0 39", "accent", 3), high))
    if kind == "ChargeState":
        plug = (line("M-33 55h12", "edge", 3) + rect(-21, 45, 21, 20, "body", "edge", 4)
                + line("M0 50h8m-8 10h8", "edge", 2.7))
        socket = (rect(80, 43, 12, 24, "muted", "edge", 3) + rect(90, 20, 54, 68, "body", "edge", 6)
                  + rect(98, 29, 38, 42, "surface", "muted", 3) + line("M106 78h22", "muted", 1.5))
        return (socket + d.show("state0", transform("translate(56 0)", plug),
                                line("M108 50h18", "muted", 2))
                + d.show("positive", transform("translate(76 0)", plug))
                + d.show("state1", ellipse(117, 50, 6, 6, "green", None))
                + d.show("state2", path("M120 34l-13 18h9l-4 12 16-21h-10Z", "green", None))
                + d.show("state3", line("M105 50l8 8 16-18", "green", 3.5))
                + d.show("state4", path("M117 36l14 25h-28Z", "surface", "danger", 2),
                         line("M117 45v7m0 4v.5", "danger", 2.5)))
    raise KeyError(kind)


def diy(d, kind):
    if kind == "ServoPosition":
        horn = (path("M90 48q6 0 6 5t-6 5l-36-3v-4Z","accent","edge")
                + ellipse(59,53,1.6,1.6,"surface",None) + ellipse(66,53,1.6,1.6,"surface",None))
        return (arc_ticks(90,53,43,180,360,8) + rect(42,69,96,7,"body","edge",2)
                + rect(54,54,72,34,"body","edge",5) + line("M59 79h61","muted",1.6)
                + d.show("known",transform(f"rotate({token('degrees')} 90 53)",horn))
                + ellipse(90,53,4.5,4.5,"surface","edge"))
    if kind == "Potentiometer":
        angle = d.slot("wiperAngle","level",-135,135)
        pins = "".join(rect(x,77,4,15,"edge",None,1) for x in (72,88,104))
        return (pins + rect(64,69,52,12,"body","edge",2) + arc_ticks(90,50,38,135,405,12)
                + ellipse(90,50,27,27,"body","edge") + ellipse(90,50,21,21,"surface","muted")
                + d.show("known",transform(f"rotate({angle} 90 50)",line("M90 46V31","accent",4))))
    if kind == "RelayModule":
        coil = line("M39 59h5v-9q0-7 4-7t4 7v11q0 4 3 4t3-4V50q0-7 4-7t4 7v9h5","edge",2)
        return (panel(20,20,140,70) + rect(31,32,46,46,"surface","muted",3) + coil
                + line("M90 62v12m39-28h13m-13 17h13","edge",2)
                + ellipse(90,62,3,3,"edge",None) + ellipse(127,46,2.5,2.5,"edge",None)
                + ellipse(127,63,2.5,2.5,"edge",None) + label(127,38,"NC",9) + label(127,78,"NO",9)
                + d.show("state0",line("M90 62l35-16","edge",3))
                + d.show("state1",line("M90 62l35 1","accent",3)))
    if kind == "RgbLed":
        lens = "M68 63V46a22 22 0 0 1 44 0v17Z"
        body = (line("M74 69v22m10-22v29m12-29v24m10-24v20","edge",2)
                + path(lens,"body","edge") + rect(65,63,50,6,"body","edge",2))
        for i, color in enumerate(("#e45559","#33b778","#efca4b","#448fec","#ce67b9","#43bfc5","#f9fbff"),1):
            body += d.show(f"state{i}",tag("path",d=lens,fill=color,stroke=paint("edge"),stroke_width=1.5))
        return body + line("M75 47q0-10 7-14","surface",2.5)
    if kind == "UltrasonicDistance":
        target_x = d.slot("targetX","level",91,153)
        board = panel(17,20,48,70)
        for y in (38,71):
            board += ellipse(41,y,14,14,"body","edge") + ellipse(41,y,9,9,"surface","muted")
            board += line(f"M35 {y-2}h12m-12 4h12","muted",1.2)
        target = rect(0,29,6,51,"body","edge",2) + line("M0 36v35","accent",2)
        return (board + d.show("known",line("M72 47q5 7 0 14m6-19q8 12 0 24","accent",1.8),
                                transform(f"translate({target_x} 0)",target)))
    if kind == "Illuminance":
        rays = "".join(transform(f"rotate({a} 126 39)",line("M126 20v-6","warm",2)) for a in range(0,360,45))
        light = d.slot("lightIntensity","level")
        return (line("M57 71v23m16-23v23","edge",2) + ellipse(65,53,24,24,"body","edge")
                + line("M49 59v-9h6v13h6V42h7v21h6V47h7v10","warm",2.8)
                + ellipse(126,39,10,10,"surface","muted")
                + d.show("known",tag("g",ellipse(126,39,10,10,"yellow","muted")+rays,opacity=light)))
    if kind == "SevenSegmentDisplay":
        segments = (
            "M76 20h26l4 4-5 4H77l-5-4Z", "M104 29l5-4 3 4-2 19-5 5-3-5Z",
            "M101 59l5-5 3 4-2 21-4 4-4-5Z", "M71 82h26l5 4-5 4H70l-4-4Z",
            "M66 59l5-5 3 5-2 19-5 5-3-4Z", "M69 30l5-5 3 5-2 18-5 5-3-5Z",
            "M74 51h26l4 4-5 4H74l-4-4Z",
        )
        pins = "".join(line(f"M46 {y}h7m74 0h7", "edge", 2) for y in (29, 43, 57, 71, 85))
        digit = pins + rect(53, 12, 74, 86, "surface", "edge", 5)
        for bit, shape in enumerate(segments):
            digit += path(shape, "body", None) + d.show(f"bit{bit}", path(shape, "danger", None))
        return digit + ellipse(116, 85, 4, 4, "body", None) + d.show("bit7", ellipse(116, 85, 4, 4, "danger", None))
    if kind == "Buzzer":
        housing = (line("M58 76v14m30-14v14", "edge", 2.4)
                   + path("M42 43q0-19 32-19t32 19v21q0 16-32 16T42 64Z")
                   + ellipse(74, 43, 32, 18, "body", "edge") + ellipse(74, 43, 10, 6, "surface", "edge")
                   + line("M50 43h7m-3-3v6", "muted", 1.2))
        return housing + d.show("state1", line("M114 43q10 12 0 24m9-32q17 20 0 40m9-48q24 28 0 56", "accent", 2.5))
    if kind == "TouchPad":
        board = panel(36, 20, 96, 70)
        for radius in (23, 17, 11):
            board += ellipse(82, 55, radius, radius, None, "muted")
        finger = path("M102 56q-4-4-2-7t7 0l5 5-3-20q0-4 3-4t4 4l3 13q8-3 13 2l3 10q1 9-6 14l-6 3-11-7Z",
                      "body", "edge")
        return (board + line("M41 49h12m-12 6h16m-16 6h12", "muted", 1.5)
                + d.show("state1", ellipse(82, 55, 23, 23, None, "accent"),
                         ellipse(82, 55, 8, 8, "accent", None), finger))
    if kind == "AirHumidity":
        sensor = (line("M42 78v13m10-13v13m10-13v13m10-13v13", "edge", 2.2)
                  + rect(32, 22, 50, 55, "body", "edge", 4))
        for y in (31, 42, 53, 64):
            sensor += rect(39, y, 15, 5, "surface", "muted", 1) + rect(61, y, 14, 5, "surface", "muted", 1)
        droplet = "M0-29C-6-18-19-4-19 7a19 19 0 0 0 38 0C19-4 6-18 0-29Z"
        size = d.slot("humidityScale", "level")
        return sensor + transform("translate(122 54)", path(droplet, "surface", "edge"),
                                  d.show("known", transform(f"scale({size})", path(droplet, "liquid", None))))
    if kind == "LoadCellScale":
        travel = d.slot("trayTravel", "level", 0, 4)
        tray = (path("M34 37h112l-8 8H42Z") + rect(80, 45, 20, 11, "body", "edge", 1)
                + d.show("positive", path("M71 37l6-15h26l6 15Z", "body", "muted"),
                         ellipse(90, 19, 5, 4, "surface", "muted")))
        base = (path("M53 56h74l14 23v7H39v-7Z") + rect(61, 63, 58, 15, "surface", "edge", 2)
                + label(90, 74, token("reading"), 11) + rect(46, 86, 9, 4) + rect(125, 86, 9, 4))
        return transform(f"translate(0 {travel})", tray) + base
    if kind == "IrBeam":
        gate = ""
        for mount in ("translate(27 0)", "translate(153 0) scale(-1 1)"):
            gate += transform(mount, rect(0, 37, 17, 34, "body", "edge", 3),
                              rect(17, 44, 5, 19, "surface", "edge", 2),
                              line("M8 71v10m-7 1h15", "edge", 2))
        return (gate + d.show("state0", line("M50 54h80", "green", 2))
                + d.show("state1", line("M50 54h32", "accent", 2),
                         path("M83 29h14v49H83Z", "body", "edge"),
                         line("M83 48v12", "warm", 3)))
    raise KeyError(kind)


def embedded(d, kind):
    if kind == "GpioBank":
        board = panel(12,25,156,61)
        for i in range(8):
            bit = 7-i
            x = 25 + 18.5*i
            board += (label(x,42,str(bit),10) + rect(x-6,48,12,17,"surface","muted",3)
                      + d.show(f"bit{bit}",rect(x-6,48,12,17,"green","edge",3))
                      + rect(x-3,71,6,7,"muted",None,1))
        return board
    if kind == "AdcChannel":
        chip = rect(18,38,40,33,"body","edge",3) + label(38,58,"ADC",11)
        for x in (25,35,45):
            chip += line(f"M{x} 32v6m0 33v6","edge",1.8)
        scale = ""
        for i in range(12):
            height = 9 + 4*i
            bar = (65+8*i,80-height,6,height)
            scale += rect(*bar,"surface","muted",1) + d.show(f"seg{i+1}",rect(*bar,"accent",None,1))
        return chip + scale
    if kind == "PwmDuty":
        traces = ""
        edges = ""
        for i in range(2):
            x = 29+61*i
            edge = d.slot(f"fall{i}","level",x,x+61)
            traces += line(f"M{x} 39H{edge}M{edge} 73H{x+61}","accent",3)
            edges += line(f"M{x} 73V39M{edge} 39V73","accent",3)
        grid = line("M29 39h122m-122 34h122M90 30v51","muted",.7)
        return (panel(18,19,144,72) + rect(25,26,130,57,"surface","muted",3) + grid
                + d.show("known",tag("g",traces+d.show("partial",edges),stroke_linecap="butt")))
    if kind == "CpuLoad":
        pins = ""
        for p in range(66,119,10):
            pins += line(f"M{p} 15v7m0 66v7","edge",2)
        for p in range(32,80,10):
            pins += line(f"M48 {p}h9m66 0h9","edge",2)
        top = d.slot("loadY","level",72,39)
        height = d.slot("loadHeight","level",0,33)
        return (pins + rect(57,22,66,66,"body","edge",5) + rect(68,32,44,46,"surface","muted",3)
                + d.show("known",rect(73,top,34,height,"accent",None,1))
                + line("M72 49h36m-36 11h36","muted",.8))
    if kind == "MemoryUsage":
        used = d.slot("usedWidth","level",0,114)
        card = path("M22 30h136v43h-6v8h-48v-6h-8v6H28v-8h-6Z")
        pads = "".join(line(f"M{x} 74v6","warm",2) for x in range(33,150,5) if x < 95 or x > 106)
        chips = "".join(rect(x,37,22,18,"surface","edge",2) for x in (34,64,94,124))
        return (card + pads + chips + rect(33,61,114,5,"surface","muted",2)
                + d.show("known",rect(33,61,used,5,"accent",None,2)))
    if kind == "BusStatus":
        nodes = ""
        for x in (34,120):
            nodes += rect(x,39,26,31,"body","edge",4) + line(f"M{x+6} 48h14m-14 7h14","muted",1.5)
        return (nodes + d.show("state0",line("M60 54h22m16 0h22","muted",2))
                + d.show("state1",line("M60 54h60","green",3),ellipse(90,54,4,4,"green",None))
                + d.show("state2",line("M60 54h20m20 0h20","muted",2),
                         line("M84 47l12 14m-12 0 12-14","danger",3)))
    if kind == "ResetCause":
        chip = (rect(28, 29, 61, 51, "body", "edge", 5)
                + ellipse(35, 36, 2, 2, "muted", None) + label(58, 61, "RST", 15)
                + line("M90 54h12", "edge", 2) + rect(102, 36, 48, 36, "surface", "muted", 4))
        for x in (39, 51, 63, 75):
            chip += line(f"M{x} 22v7m0 51v7", "edge", 2)
        for index, code in enumerate(("POR", "EXT", "SW", "WDG", "BOR")):
            chip += d.show(f"state{index}", label(126, 59, code, 14))
        return chip
    if kind == "UartRxBuffer":
        used = d.slot("bufferWidth", "level", 0, 108)
        queue = (rect(42, 23, 120, 62, "body", "edge", 5) + label(64, 40, "FIFO", 11)
                 + rect(47, 47, 110, 26, "surface", "muted", 2)
                 + d.show("known", rect(48, 48, used, 24, "accent", None, 1)))
        cells = "".join(line(f"M{x} 48v24", "muted", 1) for x in range(57, 154, 9))
        return queue + cells + line("M15 60h22m-7-7 7 7-7 7", "edge", 2) + label(24, 48, "RX", 10)
    if kind == "UartErrors":
        panel_art = panel(15, 25, 150, 60)
        for bit, code in enumerate(("PE", "FE", "ORE", "NE")):
            x = 39 + 34 * bit
            panel_art += (ellipse(x, 48, 8, 8, "surface", "muted") + label(x, 71, code, 10)
                          + d.show(f"bit{bit}", ellipse(x, 48, 8, 8, "danger", "edge"),
                                   ellipse(x-2, 46, 1.8, 1.8, "surface", None)))
        return panel_art
    if kind == "WirelessRssi":
        antenna = (line("M43 81V53m-12 28h24", "edge", 3) + ellipse(43, 47, 4, 4, "accent", None)
                   + line("M35 38q-9 9 0 18m16-18q9 9 0 18m-22-25q-16 16 0 32m28-32q16 16 0 32", "muted", 1.8))
        bars = ""
        for i, threshold in enumerate((1, 4, 7, 10)):
            x, height = 79 + 18*i, 13 + 14*i
            bar = (x, 81-height, 12, height)
            bars += rect(*bar, "surface", "muted", 2) + d.show(f"seg{threshold}", rect(*bar, "green", None, 2))
        return antenna + bars
    if kind == "RtosTaskState":
        sheet = (path("M36 20h74l13 13v51H36Z", "body", "edge") + line("M109 21v13h13", "muted", 1.5)
                 + label(61, 43, "TASK", 12) + line("M47 53h45m-45 8h38m-38 8h26", "muted", 1.7)
                 + rect(99, 45, 39, 39, "surface", "edge", 5))
        return (sheet + d.show("state0", line("M107 58h14m-14 6h20m-7 10 8-10-8-9", "accent", 2))
                + d.show("state1", path("M113 54l15 11-15 10Z", "green", None))
                + d.show("state2", line("M111 55h15m-15 19h15m-14-18q0 4 7 9-7 5-7 8m13-17q0 4-6 9 6 4 6 8", "warm", 2))
                + d.show("state3", rect(111, 55, 5, 19, "muted", None, 1), rect(122, 55, 5, 19, "muted", None, 1)))
    if kind == "I2cAddress":
        address = (rect(13, 27, 154, 53, "body", "edge", 4)
                   + line("M21 84v4h138v-4", "muted", 1.3))
        for bit in range(6, -1, -1):
            x = 20 + (6-bit) * 20
            address += (label(x+9, 43, f"A{bit}", 10) + rect(x, 48, 18, 24, "surface", "muted", 2)
                        + d.show("known", label(x+9, 65, "0", 14))
                        + d.show(f"bit{bit}", rect(x, 48, 18, 24, "body", "accent", 2), label(x+9, 65, "1", 14)))
        return address
    raise KeyError(kind)


SECTIONS = (("automotive", AUTOMOTIVE, automotive), ("diy", DIY, diy), ("embedded", EMBEDDED, embedded))
