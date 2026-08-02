// Ionity Note - Electron shell. Pushes notes to the scripture display's
// note board (HTTP on port 80) and can scan the local subnet to find it.
const { app, BrowserWindow, ipcMain } = require("electron");
const http = require("http");
const os = require("os");
const path = require("path");

function createWindow() {
  const win = new BrowserWindow({
    width: 460,
    height: 640,
    resizable: true,
    autoHideMenuBar: true,
    backgroundColor: "#0a0c1a",
    icon: path.join(__dirname, "assets", "ionity.ico"),
    webPreferences: {
      preload: path.join(__dirname, "preload.js"),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });
  win.loadFile("index.html");
}

function httpGet(url, timeoutMs) {
  return new Promise((resolve, reject) => {
    const req = http.get(url, { timeout: timeoutMs }, (res) => {
      let body = "";
      res.on("data", (chunk) => (body += chunk));
      res.on("end", () => resolve(body));
    });
    req.on("timeout", () => req.destroy(new Error("timeout")));
    req.on("error", reject);
  });
}

// Push (or clear) the note shown in the display's footer.
ipcMain.handle("push-note", async (_evt, host, text) => {
  const url = `http://${host}/note?t=${encodeURIComponent(text).replace(/%20/g, "+")}`;
  await httpGet(url, 8000);
  return true;
});

// Probe one host for the note board page.
async function probe(host) {
  try {
    const body = await httpGet(`http://${host}/`, 1500);
    return body.includes("IONITY") ? host : null;
  } catch {
    return null;
  }
}

// Scan the local /24 for the display.
ipcMain.handle("find-display", async () => {
  const nets = os.networkInterfaces();
  const bases = new Set();
  for (const name of Object.keys(nets)) {
    for (const net of nets[name] || []) {
      if (net.family === "IPv4" && !net.internal) {
        bases.add(net.address.split(".").slice(0, 3).join("."));
      }
    }
  }
  for (const base of bases) {
    const hosts = [];
    for (let i = 1; i < 255; i += 1) hosts.push(`${base}.${i}`);
    const results = await Promise.all(hosts.map(probe));
    const hit = results.find(Boolean);
    if (hit) return hit;
  }
  return null;
});

app.whenReady().then(() => {
  createWindow();
  app.on("activate", () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on("window-all-closed", () => {
  if (process.platform !== "darwin") app.quit();
});
