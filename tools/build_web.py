#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Fuegt den JavaScript-Konverter (tools/web/*) in die Web-GUI des Sketches ein
und erzeugt die eigenstaendige Datei tools/vmap-konverter.html.

Aufruf:  python3 tools/build_web.py     (nach jeder Aenderung in tools/web/)
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
INO = os.path.join(HERE, "..", "GS-Micro-NAV.ino")


def rd(n):
    return open(os.path.join(HERE, "web", n), encoding="utf-8").read()


html = rd("konverter.part.html")
js = rd("konverter.js") + "\n" + rd("konverter_ui.js")
assert ')rawliteral"' not in html + js

s = open(INO, encoding="utf-8").read()
s, n1 = re.subn(r"<!--KONV-HTML-BEGIN-->.*?<!--KONV-HTML-END-->",
                lambda m: "<!--KONV-HTML-BEGIN-->\n" + html + "<!--KONV-HTML-END-->", s, flags=re.S)
s, n2 = re.subn(r"/\*KONV-JS-BEGIN\*/.*?/\*KONV-JS-END\*/",
                lambda m: "/*KONV-JS-BEGIN*/\n" + js + "\n/*KONV-JS-END*/", s, flags=re.S)
assert n1 == 1 and n2 == 1, (n1, n2)
open(INO, "w", encoding="utf-8").write(s)

page = """<!doctype html><html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GS-Micro-NAV Konverter</title>
<style>
body{font-family:system-ui,sans-serif;background:#0b1018;color:#e6e9ef;margin:0 auto;padding:16px;max-width:640px}
h1{font-size:20px}.card{background:#131b27;border-radius:10px;padding:12px}
button{background:#2d6cdf;color:#fff;border:0;border-radius:6px;padding:7px 12px;font-size:14px}
button.d{background:#8a2c2c}button.s{background:#2a7d46}
input{background:#0b1018;color:#e6e9ef;border:1px solid #334;border-radius:6px;padding:8px;width:100%;box-sizing:border-box;margin:3px 0 8px}
progress{width:100%;margin:8px 0}small{color:#8fa0b8}
</style></head><body>
<h1>GS-Micro-NAV &ndash; Karten-Konverter</h1>
""" + html + """
<p><small>Eigenst&auml;ndige Version: Datei herunterladen und per Web-GUI des M5Dial (AP-Modus) hochladen. GS-Avionik &middot; gs-avionik.de</small></p>
<script>
const $=s=>document.querySelector(s);
const fmt=n=>n>=1048576?(n/1048576).toFixed(2)+' MB':(n/1024).toFixed(1)+' KB';
function load(){}
""" + js + """
</script></body></html>
"""
open(os.path.join(HERE, "vmap-konverter.html"), "w", encoding="utf-8").write(page)
print("OK: Sketch aktualisiert, tools/vmap-konverter.html erzeugt (%d KB JS+HTML im Sketch)"
      % ((len(html) + len(js)) // 1024))
