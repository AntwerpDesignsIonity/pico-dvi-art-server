const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("ionity", {
  pushNote: (host, text) => ipcRenderer.invoke("push-note", host, text),
  findDisplay: () => ipcRenderer.invoke("find-display"),
  localIps: () => ipcRenderer.invoke("local-ips"),
  deviceInfo: (host) => ipcRenderer.invoke("device-info", host),
  pushFirmware: (host) => ipcRenderer.invoke("push-firmware", host),
});
