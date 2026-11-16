const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { app, BrowserWindow, Menu, ipcMain, session } = require('electron');
const { createViewerUrl } = require('./connection-utils.cjs');
const LAUNCHER_FILE = pathToFileURL(path.join(__dirname, 'launcher.html')).href;
let mainWindow = null;
let activeTunnelOrigin = null;

function isLauncherUrl(value) {
  try {
    const target = new URL(value);
    const launcher = new URL(LAUNCHER_FILE);
    return target.protocol === 'file:'
      && target.pathname.toLowerCase() === launcher.pathname.toLowerCase();
  } catch {
    return false;
  }
}

function navigationIsAllowed(target) {
  if (isLauncherUrl(target)) return true;
  if (!activeTunnelOrigin) return false;
  try {
    return new URL(target).origin === activeTunnelOrigin;
  } catch {
    return false;
  }
}

function createWindow() {
  Menu.setApplicationMenu(null);
  mainWindow = new BrowserWindow({
    width: 1024,
    height: 790,
    minWidth: 720,
    minHeight: 650,
    backgroundColor: '#f4f7f6',
    title: 'Экран рядом · Windows viewer',
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      webSecurity: true,
      allowRunningInsecureContent: false,
      spellcheck: false,
      webviewTag: false,
    },
  });

  mainWindow.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  const guardNavigation = (event, target) => {
    if (!navigationIsAllowed(target)) event.preventDefault();
  };
  mainWindow.webContents.on('will-navigate', guardNavigation);
  mainWindow.webContents.on('will-redirect', guardNavigation);
  mainWindow.webContents.on('will-attach-webview', (event) => event.preventDefault());
  mainWindow.on('closed', () => {
    mainWindow = null;
    activeTunnelOrigin = null;
  });

  void mainWindow.loadFile(path.join(__dirname, 'launcher.html'));
}

ipcMain.handle('connect-session', async (event, values) => {
  if (!event.senderFrame?.url || !isLauncherUrl(event.senderFrame.url)) {
    return { ok: false, error: 'Откройте подключение из стартового окна приложения.' };
  }

  if (!mainWindow || mainWindow.isDestroyed()) {
    return { ok: false, error: 'Окно приложения закрыто.' };
  }

  try {
    const viewerSession = createViewerUrl(values?.url, values?.code, values?.name);
    activeTunnelOrigin = viewerSession.origin;
    const windowToLoad = mainWindow;
    void windowToLoad.loadURL(viewerSession.url).catch(() => {
      if (mainWindow !== windowToLoad || windowToLoad.isDestroyed()) return;
      activeTunnelOrigin = null;
      windowToLoad.webContents.once('did-finish-load', () => {
        if (mainWindow === windowToLoad && !windowToLoad.isDestroyed()) {
          windowToLoad.webContents.send('connect-failed', 'Не удалось открыть страницу подключения. Проверьте ссылку и интернет.');
        }
      });
      void windowToLoad.loadFile(path.join(__dirname, 'launcher.html'));
    });
    return { ok: true };
  } catch (error) {
    return { ok: false, error: error.message || 'Не удалось начать подключение.' };
  }
});

app.whenReady().then(() => {
  const allowViewerFullscreen = (webContents, permission) => {
    if (permission !== 'fullscreen' || !activeTunnelOrigin) return false;
    try {
      return new URL(webContents.getURL()).origin === activeTunnelOrigin;
    } catch {
      return false;
    }
  };
  session.defaultSession.setPermissionRequestHandler((webContents, permission, callback) => {
    callback(allowViewerFullscreen(webContents, permission));
  });
  session.defaultSession.setPermissionCheckHandler((webContents, permission) => {
    return allowViewerFullscreen(webContents, permission);
  });
  createWindow();
  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
