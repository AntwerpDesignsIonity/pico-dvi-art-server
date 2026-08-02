// Ionity Pico Note-Display - Electron shell. Pushes notes to the scripture display's
// note board (HTTP on port 80) and can scan the local subnet to find it.
const { app, BrowserWindow, ipcMain, dialog } = require("electron");
const fs = require("fs");
const http = require("http");
const os = require("os");
const path = require("path");
const zlib = require("zlib");

const DEVICE_NAME = "ionity-scripture.local";

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
    const body = await httpGet(`http://${host}/id`, 1500);
    const info = JSON.parse(body);
    if (info && info.device === "ionity-scripture") return host;
  } catch {
    try {
      const body = await httpGet(`http://${host}/`, 1500);
      return body.includes("IONITY") ? host : null;
    } catch {
      return null;
    }
  }
}

async function probeList(hosts, batchSize) {
  for (let i = 0; i < hosts.length; i += batchSize) {
    const slice = hosts.slice(i, i + batchSize);
    const results = await Promise.all(slice.map(probe));
    const hit = results.find(Boolean);
    if (hit) return hit;
  }
  return null;
}

// This machine's own IPv4 addresses, for the footer.
ipcMain.handle("local-ips", () => {
  const nets = os.networkInterfaces();
  const ips = [];
  for (const name of Object.keys(nets)) {
    for (const net of nets[name] || []) {
      if (net.family === "IPv4" && !net.internal) ips.push(net.address);
    }
  }
  return ips;
});

// Query the device identity (V3+ answers /id with JSON).
ipcMain.handle("device-info", async (_evt, host) => {
  try {
    const body = await httpGet(`http://${host}/id`, 3000);
    return JSON.parse(body);
  } catch {
    return null;
  }
});

// Push a firmware .bin to the device's OTA endpoint (V3+ firmware).
ipcMain.handle("push-firmware", async (_evt, host) => {
  const picked = await dialog.showOpenDialog({
    title: "Select firmware image (.bin)",
    filters: [{ name: "Firmware image", extensions: ["bin"] }],
    properties: ["openFile"],
  });
  if (picked.canceled || !picked.filePaths.length) return { status: "cancelled" };

  const image = fs.readFileSync(picked.filePaths[0]);
  const crc = zlib.crc32(image) >>> 0;
  const crcHex = crc.toString(16).padStart(8, "0");

  return new Promise((resolve, reject) => {
    const req = http.request(
      {
        host,
        port: 80,
        method: "POST",
        path: `/fw?len=${image.length}&crc=${crcHex}`,
        headers: { "Content-Length": image.length },
        timeout: 180000,
      },
      (res) => {
        let body = "";
        res.on("data", (c) => (body += c));
        res.on("end", () =>
          resolve({ status: res.statusCode === 200 ? "staged" : "rejected", body })
        );
      }
    );
    req.on("timeout", () => req.destroy(new Error("timeout")));
    req.on("error", reject);
    req.end(image);
  });
});

// Scan the local /24 for the display.
ipcMain.handle("find-display", async () => {
  const hostnameHit = await probe(DEVICE_NAME);
  if (hostnameHit) return hostnameHit;

  const nets = os.networkInterfaces();
  const bases = new Set();
  for (const name of Object.keys(nets)) {
    for (const net of nets[name] || []) {
      if (net.family === "IPv4" && !net.internal) {
        bases.add(net.address.split(".").slice(0, 3).join("."));
      }
    }
  }
  const hosts = [];
  for (const base of bases) {
    for (let i = 1; i < 255; i += 1) hosts.push(`${base}.${i}`);
  }
  return probeList(hosts, 16);
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
