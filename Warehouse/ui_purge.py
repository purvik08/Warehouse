import re

path = r'server2/server2.ino'
with open(path, 'r', encoding='utf-8') as f:
    orig = f.read()

# Remove the lineFollowers and roboticArms HTML inputs
orig = re.sub(r'<div style="margin-bottom: 1\.5rem;">\s*<label[^>]*>Registered Line Followers</label>\s*<input type="number" id="lineFollowers"[^>]*>\s*</div>', '', orig)
orig = re.sub(r'<div style="margin-bottom: 2rem;">\s*<label[^>]*>Registered Robotic Arms</label>\s*<input type="number" id="roboticArms"[^>]*>\s*</div>', '', orig)

# Remove JS
js_old = """        const settings = {
          team: document.getElementById('teamName').value,
          lineFollowers: parseInt(document.getElementById('lineFollowers').value),
          roboticArms: parseInt(document.getElementById('roboticArms').value)
        };"""
js_new = """        const settings = {
          team: document.getElementById('teamName').value
        };"""
orig = orig.replace(js_old, js_new)

# Add a read-only display to replace them in settings if preferred, or just rely on dashboard.
# I'll just remove them cleanly.

with open(path, 'w', encoding='utf-8') as f:
    f.write(orig)

print("UI settings correctly decoupled")
