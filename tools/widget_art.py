"""Shared SVG primitives and metadata for the device catalogs."""

import math
from xml.sax.saxutils import escape, quoteattr


def number(kind, name, en, unit, limits, value, decimals=0, motion="none"):
    return dict(type=kind, name=name, en=en, unit=unit, minimum=limits[0], maximum=limits[1],
                value=value, decimals=decimals, mode="number", motion=motion, states=[], statesEn=[])


def state(kind, name, en, labels, english, value, motion="none", bits=False):
    entry = number(kind, name, en, "", (0, len(labels) - 1), value, motion=motion)
    entry.update(mode="bits" if bits else "state", states=labels, statesEn=english)
    return entry


def flags(kind, name, en, labels, english, clear, clear_en, value):
    entry = number(kind, name, en, "", (0, (1 << len(labels)) - 1), value)
    entry.update(mode="flags", flags=labels, flagsEn=english, clearLabel=clear, clearLabelEn=clear_en)
    return entry


def token(name):
    return "{{" + name + "}}"


def tag(name, inner="", **attrs):
    attributes = "".join(" " + k.replace("_", "-") + "=" + quoteattr(str(v)) for k, v in attrs.items())
    return f"<{name}{attributes}>{inner}</{name}>" if inner else f"<{name}{attributes}/>"


def paint(color):
    return "none" if color is None else token(color)


def rect(x, y, w, h, fill="body", stroke="edge", r=0):
    return tag("rect", x=x, y=y, width=w, height=h, rx=r, fill=paint(fill), stroke=paint(stroke))


def ellipse(x, y, rx, ry, fill="body", stroke="edge"):
    return tag("ellipse", cx=x, cy=y, rx=rx, ry=ry, fill=paint(fill), stroke=paint(stroke))


def path(d, fill="body", stroke="edge", width=1.5):
    return tag("path", d=d, fill=paint(fill), stroke=paint(stroke), stroke_width=width)


def line(d, color="muted", width=1.4):
    return path(d, None, color, width)


def label(x, y, value, size=12):
    return tag("text", escape(value), x=x, y=y, fill=paint("ink"), stroke="none",
               text_anchor="middle", font_family="sans-serif", font_size=size)


def transform(transform_spec, *content):
    return tag("g", "".join(content), transform=transform_spec)


def panel(x, y, w, h):
    screws = "".join(ellipse(xx, yy, 1.2, 1.2, "muted", None) for xx in (x+5, x+w-5) for yy in (y+5,y+h-5))
    return rect(x,y,w,h,"body","edge",5) + screws


def arc_ticks(x,y,r,lo,hi,count):
    marks = []
    for n in range(count+1):
        a = math.radians(lo+(hi-lo)*n/count)
        length = 5 if n%2==0 else 3
        x1, y1 = x+(r-length)*math.cos(a), y+(r-length)*math.sin(a)
        x2, y2 = x+r*math.cos(a), y+r*math.sin(a)
        marks.append(line(f"M{x1:.2f} {y1:.2f}L{x2:.2f} {y2:.2f}","edge",1.3 if n%2==0 else 1))
    return "".join(marks)
