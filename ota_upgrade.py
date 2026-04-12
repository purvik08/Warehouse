import re

# 1. Server2
path_sv = r'server2/server2.ino'
with open(path_sv, 'r', encoding='utf-8') as f:
    orig = f.read()

orig = orig.replace('#include <LittleFS.h>', '#include <LittleFS.h>\n#include <ElegantOTA.h>')
orig = orig.replace('server.begin();', 'server.begin();\n  ElegantOTA.begin(&server);')
orig = orig.replace('server.handleClient();', 'server.handleClient();\n  ElegantOTA.loop();')

old_btn = '<button onclick="saveSettings()" style="width: 100%;">Sync Configuration</button>'
new_btn = """<button onclick="saveSettings()" style="width: 100%; margin-bottom: 1rem;">Sync Configuration</button>
        <button onclick="window.location.href=\'/update\'" class="danger" style="width: 100%;">System OTA Update</button>"""
orig = orig.replace(old_btn, new_btn)

with open(path_sv, 'w', encoding='utf-8') as f:
    f.write(orig)

# 2. Mobile Robot
path_mr = r'MobileRobot/MobileRobot.ino'
with open(path_mr, 'r', encoding='utf-8') as f:
    orig = f.read()

orig = orig.replace('#include <WiFi.h>', '#include <WiFi.h>\n#include <WebServer.h>\n#include <ElegantOTA.h>\n\nWebServer otaServer(80);')
orig = orig.replace('connectToServerAP();', 'connectToServerAP();\n  \n  otaServer.begin();\n  ElegantOTA.begin(&otaServer);')
orig = orig.replace('maintainConnection();', 'maintainConnection();\n  \n  otaServer.handleClient();\n  ElegantOTA.loop();')

with open(path_mr, 'w', encoding='utf-8') as f:
    f.write(orig)

# 3. Robotic Arm
path_ra = r'RoboticArm/RoboticArm.ino'
with open(path_ra, 'r', encoding='utf-8') as f:
    orig = f.read()

orig = orig.replace('#include <WiFi.h>', '#include <WiFi.h>\n#include <WebServer.h>\n#include <ElegantOTA.h>\n\nWebServer otaServer(80);')
orig = orig.replace('connectToServerAP();', 'connectToServerAP();\n  \n  otaServer.begin();\n  ElegantOTA.begin(&otaServer);')
orig = orig.replace('if (WiFi.status() != WL_CONNECTED) {', 'otaServer.handleClient();\n  ElegantOTA.loop();\n\n  if (WiFi.status() != WL_CONNECTED) {')

with open(path_ra, 'w', encoding='utf-8') as f:
    f.write(orig)

print("OTA injection completed successfully.")
