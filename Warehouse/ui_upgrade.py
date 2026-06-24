import re

path = r'server2/server2.ino'
with open(path, 'r', encoding='utf-8') as f:
    orig_content = f.read()

# Common style header
nav_head = """  <!DOCTYPE html>
  <html>
  <head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <style>
      :root {
        --bg-dark: #0f172a;
        --bg-card: rgba(30, 41, 59, 0.7);
        --text-main: #f8fafc;
        --text-muted: #94a3b8;
        --accent: #38bdf8;
        --accent-hover: #0ea5e9;
        --success: #10b981;
        --danger: #ef4444;
        --border: rgba(255, 255, 255, 0.1);
      }
      body { 
        font-family: system-ui, -apple-system, sans-serif; 
        margin: 0; 
        padding: 0; 
        background-color: var(--bg-dark); 
        color: var(--text-main);
        min-height: 100vh;
      }
      header { 
        padding: 2rem; 
        text-align: center; 
        background: linear-gradient(to right, rgba(15, 23, 42, 0.9), rgba(30, 41, 59, 0.9));
        border-bottom: 1px solid var(--border);
      }
      h1, h2 { margin: 0 0 1rem 0; font-weight: 600; }
      h1 { background: -webkit-linear-gradient(45deg, #38bdf8, #818cf8); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
      nav { 
        display: flex; justify-content: center; gap: 1rem; padding: 1rem;
        background: var(--bg-card); backdrop-filter: blur(12px);
        border-bottom: 1px solid var(--border); position: sticky; top: 0; z-index: 100; flex-wrap: wrap;
      }
      nav a {
        color: var(--text-muted); text-decoration: none; padding: 0.75rem 1.5rem;
        border-radius: 8px; font-weight: 500; transition: all 0.3s ease; border: 1px solid transparent;
      }
      nav a:hover, nav a.active { 
        color: #fff; background: rgba(56, 189, 248, 0.1);
        border: 1px solid var(--accent); box-shadow: 0 0 15px rgba(56, 189, 248, 0.2);
      }
      section { padding: 2rem; max-width: 1200px; margin: 0 auto; }
      .card {
        background: var(--bg-card); backdrop-filter: blur(12px);
        border: 1px solid var(--border); border-radius: 16px; padding: 1.5rem;
        box-shadow: 0 10px 30px -10px rgba(0,0,0,0.5); transition: transform 0.3s ease;
      }
      .card:hover { transform: translateY(-5px); }
      
      .status-item { display: flex; justify-content: space-between; align-items: center; padding: 1rem 0; border-bottom: 1px solid var(--border); }
      .status-item:last-child { border-bottom: none; }
      .status-value { font-weight: bold; color: var(--accent); font-size: 1.2rem; }
      
      button {
        background: linear-gradient(135deg, var(--accent), var(--accent-hover)); border: none;
        padding: 0.75rem 1.5rem; color: white; font-weight: 600; border-radius: 8px; cursor: pointer;
        transition: all 0.3s ease; box-shadow: 0 4px 12px rgba(56, 189, 248, 0.3);
      }
      button:hover { transform: translateY(-2px); box-shadow: 0 6px 16px rgba(56, 189, 248, 0.5); }
      
      input {
        width: 100%; padding: 1rem; background: rgba(15, 23, 42, 0.5); border: 1px solid var(--border);
        border-radius: 8px; color: white; box-sizing: border-box; transition: all 0.3s ease;
      }
      input:focus { outline: none; border-color: var(--accent); box-shadow: 0 0 0 2px rgba(56, 189, 248, 0.2); }
      
      .log { 
        font-family: 'Courier New', Courier, monospace; font-size: 0.9rem;
        white-space: pre-wrap; background: #000; color: #10b981; padding: 1rem;
        border-radius: 8px; max-height: 300px; overflow-y: auto; border: 1px solid #333;
      }
    </style>
  </head>
  <body>
"""

nav_footer = """  </body>
  </html>"""

# 1. serveWebInterface (Dashboard)
dashboard_html = nav_head + """
    <header><h1>Warehouse Automation - %TEAM_NAME%</h1></header>
    <nav>
      <a href="/" class="active">Dashboard</a>
      <a href="/inventory">Inventory</a>
      <a href="/robot">Robot</a>
      <a href="/settings">Settings</a>
    </nav>
    <section>
      <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 2rem;">
        <div class="card">
          <h2>System Status</h2>
          <div class="status-item"><span>Connected Devices</span><span class="status-value" id="deviceCount">0</span></div>
          <div class="status-item"><span>Line Followers</span><span class="status-value">%LINE_FOLLOWERS%</span></div>
          <div class="status-item"><span>Robotic Arms</span><span class="status-value">%ROBOTIC_ARMS%</span></div>
          <div class="status-item"><span>Free Memory</span><span class="status-value" id="freeMemory">-</span></div>
        </div>
        <div class="card">
          <h2>Quick Inventory</h2>
          <div class="status-item"><span>Rack A</span><span class="status-value" id="rackA-status">%RACKA_COUNT%/%RACKA_CAPACITY%</span></div>
          <div class="status-item"><span>Rack B</span><span class="status-value" id="rackB-status">%RACKB_COUNT%/%RACKB_CAPACITY%</span></div>
          <div class="status-item"><span>Rack C</span><span class="status-value" id="rackC-status">%RACKC_COUNT%/%RACKC_CAPACITY%</span></div>
        </div>
      </div>
      <div class="card" style="margin-top: 2rem;">
        <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 1rem;">
          <h2 style="margin: 0;">Live Network Logs</h2>
          <button onclick="clearLogs()" style="border-radius: 20px; padding: 0.5rem 1rem;">Clear</button>
        </div>
        <div class="log" id="logOutput"></div>
      </div>
    </section>

  <script>
    let ws;
    let logOutput = document.getElementById('logOutput');

    function connectWebSocket() {
      ws = new WebSocket(`ws://${location.hostname}:81`);
      ws.onopen = () => log("Network connection established.");
      ws.onmessage = (e) => {
        try {
          const d = JSON.parse(e.data);
          if (d.type === 'rfid') log(">>> RFID Tag: " + d.tag);
          else if (d.type === 'inventory') { updateInventoryStatus(d); log(">>> Grid Inventory Sync."); }
          else if (d.type === 'location') log(">>> Localization Ping: " + d.tag);
          else if (d.type === 'arm') log(">>> Arm Event: " + JSON.stringify(d));
          else if (d.type === 'status') updateDeviceStatus(d);
        } catch (err) { log("RAW: " + e.data); }
      };
      ws.onclose = () => { log("Connection lost. Reconnecting..."); setTimeout(connectWebSocket, 3000); };
    }

    function log(msg) {
      logOutput.textContent += `[${new Date().toLocaleTimeString()}] ${msg}\\n`;
      logOutput.scrollTop = logOutput.scrollHeight;
    }
    function clearLogs() { logOutput.textContent = ''; }

    function updateInventoryStatus(data) {
      if(data.rackA !== undefined) document.getElementById('rackA-status').textContent = `${data.rackA}/%RACKA_CAPACITY%`;
      if(data.rackB !== undefined) document.getElementById('rackB-status').textContent = `${data.rackB}/%RACKB_CAPACITY%`;
      if(data.rackC !== undefined) document.getElementById('rackC-status').textContent = `${data.rackC}/%RACKC_CAPACITY%`;
    }

    function updateDeviceStatus(data) {
      if(data.connectedDevices) document.getElementById('deviceCount').textContent = data.connectedDevices.length;
      if(data.freeHeap) document.getElementById('freeMemory').textContent = Math.round(data.freeHeap/1024) + " KB";
    }

    function fetchStatus() {
      fetch('/api/status').then(res => res.json()).then(data => {
        updateDeviceStatus(data); updateInventoryStatus(data.inventory);
      }).catch(err => log("Status fetch error: " + err));
    }
    
    window.onload = () => { connectWebSocket(); fetchStatus(); setInterval(fetchStatus, 5000); };
  </script>
""" + nav_footer


# 2. serveRobotPage
robot_html = nav_head + """
    <header><h1>Robotic Arm Console</h1></header>
    <nav>
      <a href="/">Dashboard</a>
      <a href="/inventory">Inventory</a>
      <a href="/robot" class="active">Robot</a>
      <a href="/settings">Settings</a>
    </nav>
    <section>
      <div class="card" style="max-width: 600px; margin: 0 auto;">
        <h2>Command Matrix</h2>
        <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; margin-bottom: 2rem;">
          <button onclick="sendCommand('pick')" style="padding: 1.5rem; font-size: 1.1rem;">Pick Item</button>
          <button onclick="sendCommand('place')" style="padding: 1.5rem; font-size: 1.1rem;">Place Item</button>
          <button onclick="sendCommand('home')" style="grid-column: span 2; padding: 1.5rem; background: linear-gradient(135deg, #8b5cf6, #d946ef);">Return Home</button>
        </div>
        
        <h2>Terminal Injection</h2>
        <div style="display: flex; gap: 1rem; margin-bottom: 2rem;">
          <input type="text" id="customCommand" placeholder="Enter remote instruction snippet...">
          <button onclick="sendCustomCommand()">Execute</button>
        </div>
        
        <h2>Arm Telemetry</h2>
        <div id="commandLog" class="log" style="height: 150px;"></div>
      </div>
    </section>

    <script>
      function sendCommand(cmd) {
        fetch('/api/command', { method: 'POST', body: cmd })
        .then(() => logMessage(`Tx -> ${cmd}`))
        .catch(err => logMessage(`ERR -> ${err}`));
      }

      function sendCustomCommand() {
        const i = document.getElementById('customCommand');
        if(i.value.trim()){ sendCommand(i.value); i.value = ''; }
      }

      function logMessage(msg) {
        const l = document.getElementById('commandLog');
        l.textContent += `[${new Date().toLocaleTimeString()}] ${msg}\\n`;
        l.scrollTop = l.scrollHeight;
      }

      const ws = new WebSocket(`ws://${location.hostname}:81`);
      ws.onmessage = (e) => {
        const d = JSON.parse(e.data);
        if(d.type === 'arm' || d.type === 'notification') logMessage(`Rx <- ${JSON.stringify(d)}`);
      };
    </script>
""" + nav_footer

# 3. serveSettingsPage
settings_html = nav_head + """
    <header><h1>System Parameters</h1></header>
    <nav>
      <a href="/">Dashboard</a>
      <a href="/inventory">Inventory</a>
      <a href="/robot">Robot</a>
      <a href="/settings" class="active">Settings</a>
    </nav>
    <section>
      <div class="card" style="max-width: 500px; margin: 0 auto;">
        <div style="margin-bottom: 1.5rem;">
          <label style="display: block; margin-bottom: 0.5rem; color: var(--text-muted);">Team Designation / Cluster ID</label>
          <input type="text" id="teamName" value="%TEAM_NAME%">
        </div>
        <div style="margin-bottom: 1.5rem;">
          <label style="display: block; margin-bottom: 0.5rem; color: var(--text-muted);">Registered Line Followers</label>
          <input type="number" id="lineFollowers" min="1" max="10" value="%LINE_FOLLOWERS%">
        </div>
        <div style="margin-bottom: 2rem;">
          <label style="display: block; margin-bottom: 0.5rem; color: var(--text-muted);">Registered Robotic Arms</label>
          <input type="number" id="roboticArms" min="1" max="5" value="%ROBOTIC_ARMS%">
        </div>
        
        <button onclick="saveSettings()" style="width: 100%;">Sync Configuration</button>
        
        <div id="statusMessage" style="margin-top: 1rem; padding: 1rem; border-radius: 8px; display: none; text-align: center; font-weight: bold;"></div>
      </div>
    </section>

    <script>
      function saveSettings() {
        const settings = {
          team: document.getElementById('teamName').value,
          lineFollowers: parseInt(document.getElementById('lineFollowers').value),
          roboticArms: parseInt(document.getElementById('roboticArms').value)
        };
        fetch('/api/settings', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(settings) })
        .then(() => showStatus('Parametric sync successful!', 'success'))
        .catch(err => showStatus('Sync failed: ' + err, 'error'));
      }

      function showStatus(msg, type) {
        const d = document.getElementById('statusMessage');
        d.textContent = msg; d.style.display = 'block';
        d.style.backgroundColor = type === 'success' ? 'rgba(16, 185, 129, 0.2)' : 'rgba(239, 68, 68, 0.2)';
        d.style.color = type === 'success' ? '#34d399' : '#f87171';
        d.style.border = type === 'success' ? '1px solid #059669' : '1px solid #dc2626';
        setTimeout(() => d.style.display = 'none', 3000);
      }
    </script>
""" + nav_footer

# 4. serveInventoryPage
inventory_html = nav_head + """
    <header><h1>Visual Storage Manifest</h1></header>
    <nav>
      <a href="/">Dashboard</a>
      <a href="/inventory" class="active">Inventory</a>
      <a href="/robot">Robot</a>
      <a href="/settings">Settings</a>
    </nav>
    <section>
      <div style="display: flex; flex-direction: column; gap: 2rem; max-width: 800px; margin: 0 auto;">
        
        <div class="card">
          <div style="display: flex; justify-content: space-between; align-items: baseline;">
            <h2>Sector Alpha</h2>
            <div style="color: var(--text-muted);"><span id="rackA-count" style="color: var(--text-main); font-size: 1.5rem; font-weight: bold;">%RACKA_COUNT%</span> / %RACKA_CAPACITY%</div>
          </div>
          <div style="height: 24px; background: rgba(0,0,0,0.5); border-radius: 12px; margin-top: 1rem; overflow: hidden; border: 1px solid var(--border); box-shadow: inset 0 2px 5px rgba(0,0,0,0.5);">
            <div id="rackA-bar" style="height: 100%; width: %RACKA_PERCENT%%; background: linear-gradient(90deg, #10b981, #3b82f6); transition: width 0.8s cubic-bezier(0.4, 0, 0.2, 1); border-radius: 12px; box-shadow: 0 0 10px rgba(59, 130, 246, 0.8);"></div>
          </div>
        </div>

        <div class="card">
          <div style="display: flex; justify-content: space-between; align-items: baseline;">
            <h2>Sector Bravo</h2>
            <div style="color: var(--text-muted);"><span id="rackB-count" style="color: var(--text-main); font-size: 1.5rem; font-weight: bold;">%RACKB_COUNT%</span> / %RACKB_CAPACITY%</div>
          </div>
          <div style="height: 24px; background: rgba(0,0,0,0.5); border-radius: 12px; margin-top: 1rem; overflow: hidden; border: 1px solid var(--border); box-shadow: inset 0 2px 5px rgba(0,0,0,0.5);">
            <div id="rackB-bar" style="height: 100%; width: %RACKB_PERCENT%%; background: linear-gradient(90deg, #8b5cf6, #d946ef); transition: width 0.8s cubic-bezier(0.4, 0, 0.2, 1); border-radius: 12px; box-shadow: 0 0 10px rgba(217, 70, 239, 0.8);"></div>
          </div>
        </div>

        <div class="card">
          <div style="display: flex; justify-content: space-between; align-items: baseline;">
            <h2>Sector Charlie</h2>
            <div style="color: var(--text-muted);"><span id="rackC-count" style="color: var(--text-main); font-size: 1.5rem; font-weight: bold;">%RACKC_COUNT%</span> / %RACKC_CAPACITY%</div>
          </div>
          <div style="height: 24px; background: rgba(0,0,0,0.5); border-radius: 12px; margin-top: 1rem; overflow: hidden; border: 1px solid var(--border); box-shadow: inset 0 2px 5px rgba(0,0,0,0.5);">
            <div id="rackC-bar" style="height: 100%; width: %RACKC_PERCENT%%; background: linear-gradient(90deg, #f59e0b, #ef4444); transition: width 0.8s cubic-bezier(0.4, 0, 0.2, 1); border-radius: 12px; box-shadow: 0 0 10px rgba(239, 68, 68, 0.8);"></div>
          </div>
        </div>
        
      </div>
    </section>

    <script>
      const ws = new WebSocket(`ws://${location.hostname}:81`);
      ws.onmessage = (e) => {
        const d = JSON.parse(e.data);
        if(d.type === 'inventory') {
          if(d.rackA !== undefined) {
             document.getElementById('rackA-count').textContent = d.rackA;
             document.getElementById('rackA-bar').style.width = Math.min(100, (d.rackA/%RACKA_CAPACITY%)*100) + '%';
          }
          if(d.rackB !== undefined) {
             document.getElementById('rackB-count').textContent = d.rackB;
             document.getElementById('rackB-bar').style.width = Math.min(100, (d.rackB/%RACKB_CAPACITY%)*100) + '%';
          }
          if(d.rackC !== undefined) {
             document.getElementById('rackC-count').textContent = d.rackC;
             document.getElementById('rackC-bar').style.width = Math.min(100, (d.rackC/%RACKC_CAPACITY%)*100) + '%';
          }
        }
      };
    </script>
""" + nav_footer

def replace_fn_html(fn_name, new_html, text):
    # Regex designed to find the rawliteral block precisely within a specific function
    # It finds void functionName() { ... String html = R"rawliteral(...)rawliteral"; ... }
    
    # We locate the function declaration
    sn = text.find(f"void {fn_name}()")
    
    if sn == -1:
        return text

    # We find the start of the RAWLITERAL
    rl_start = text.find('R"rawliteral(', sn)
    if rl_start == -1:
        return text
        
    start_inner = rl_start + len('R"rawliteral(')
    
    # End of RAWLITERAL
    rl_end = text.find(')rawliteral"', start_inner)
    if rl_end == -1:
        return text
        
    # Substitute
    new_text = text[:start_inner] + "\n" + new_html + "\n  " + text[rl_end:]
    return new_text

orig_content = replace_fn_html('serveWebInterface', dashboard_html, orig_content)
orig_content = replace_fn_html('serveRobotPage', robot_html, orig_content)
orig_content = replace_fn_html('serveSettingsPage', settings_html, orig_content)
orig_content = replace_fn_html('serveInventoryPage', inventory_html, orig_content)

with open(path, 'w', encoding='utf-8') as f:
    f.write(orig_content)

print("UI rewrite completed successfully.")
