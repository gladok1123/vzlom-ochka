const { contextBridge, ipcRenderer } = require('electron');

const isLauncherPage = (() => {
  try {
    const page = new URL(location.href);
    return page.protocol === 'file:' && page.pathname.toLowerCase().endsWith('/launcher.html');
  } catch {
    return false;
  }
})();

if (isLauncherPage) {
  contextBridge.exposeInMainWorld('screenBridge', {
    connectSession: (values) => ipcRenderer.invoke('connect-session', values),
    onConnectFailed: (callback) => {
      if (typeof callback !== 'function') return () => {};
      const listener = (_event, message) => callback(String(message));
      ipcRenderer.on('connect-failed', listener);
      return () => ipcRenderer.removeListener('connect-failed', listener);
    },
  });
}
