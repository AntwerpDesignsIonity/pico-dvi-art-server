const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("ionity", {
  pushNote: (host, text) => ipcRenderer.invoke("push-note", host, text),
  findDisplay: () => ipcRenderer.invoke("find-display"),
});
