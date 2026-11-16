import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import process from 'node:process';
import { createRemoteSupportServer } from './src/server.js';

const localOnly = process.argv.includes('--local-only');
const noOpen = process.argv.includes('--no-open');
const configuredPort = Number(process.env.PORT ?? 4173);

if (!Number.isInteger(configuredPort) || configuredPort < 0 || configuredPort > 65_535) {
  console.error('PORT должен быть числом от 0 до 65535.');
  process.exit(1);
}

const app = createRemoteSupportServer();
const tunnelAddress = (port) => `http://127.0.0.1:${port}`;
let tunnelProcess = null;
let shuttingDown = false;

function openBrowser(url) {
  let command;
  let args;
  if (process.platform === 'win32') {
    command = 'cmd.exe';
    args = ['/d', '/s', '/c', 'start', '', url];
  } else if (process.platform === 'darwin') {
    command = 'open';
    args = [url];
  } else {
    command = 'xdg-open';
    args = [url];
  }

  const browser = spawn(command, args, { detached: true, stdio: 'ignore', windowsHide: true });
  browser.on('error', () => console.log(`Откройте панель вручную: ${url}`));
  browser.unref();
}

function startCloudflareTunnel(origin) {
  console.log('\nЗапускаю Cloudflare Quick Tunnel…');
  try {
    tunnelProcess = spawn('cloudflared', ['tunnel', '--url', origin], {
      stdio: ['ignore', 'pipe', 'pipe'],
      windowsHide: true,
    });
  } catch (error) {
    console.error(`Не удалось запустить cloudflared: ${error.message}`);
    console.error('Установите cloudflared и добавьте его в PATH; локальная панель продолжит работать.');
    return;
  }

  let tunnelReady = false;
  const handleOutput = (stream) => {
    const lines = createInterface({ input: stream, crlfDelay: Infinity });
    lines.on('line', (line) => {
      const match = line.match(/https:\/\/[a-z0-9-]+\.trycloudflare\.com/i);
      if (match && !tunnelReady) {
        tunnelReady = true;
        const publicUrl = match[0];
        try {
          app.setPublicUrl(publicUrl);
          console.log(`Туннель готов: ${publicUrl}`);
          console.log('Передайте участнику ссылку выше и код просмотра из начала вывода.');
        } catch (error) {
          console.error(`Не удалось принять адрес туннеля: ${error.message}`);
        }
        return;
      }
      if (/\b(error|failed|failure|unable|denied)\b/i.test(line)) {
        console.error(`[cloudflared] ${line}`);
      }
    });
  };

  handleOutput(tunnelProcess.stdout);
  handleOutput(tunnelProcess.stderr);

  tunnelProcess.on('error', (error) => {
    if (error.code === 'ENOENT') {
      console.error('\ncloudflared не найден в PATH. Установите Cloudflare cloudflared, затем запустите npm start снова.');
    } else {
      console.error(`Ошибка запуска cloudflared: ${error.message}`);
    }
    console.error('Локальная панель останется доступна; удалённое подключение без туннеля работать не будет.');
  });

  tunnelProcess.on('exit', (code, signal) => {
    tunnelProcess = null;
    if (!shuttingDown && (code !== 0 || signal)) {
      app.setPublicUrl(null);
      console.error(`Cloudflare Tunnel завершился (${signal ?? `код ${code}`}). Локальная панель продолжает работать.`);
    }
  });
}

async function start() {
  await new Promise((resolve, reject) => {
    const onError = (error) => reject(error);
    app.server.once('error', onError);
    app.server.listen(configuredPort, '127.0.0.1', () => {
      app.server.off('error', onError);
      resolve();
    });
  });

  const address = app.server.address();
  const localUrl = tunnelAddress(address.port);
  const hostUrl = `${localUrl}/host?key=${encodeURIComponent(app.hostKey)}`;

  console.log('\n┌────────────────────────────────────────┐');
  console.log('│          ЭКРАН РЯДОМ · Share            │');
  console.log('└────────────────────────────────────────┘');
  console.log(`Локальная панель владельца: ${hostUrl}`);
  console.log(`Код просмотра:              ${app.sessionCode}`);
  console.log('Не отправляйте ссылку локальной панели: в ней находится ключ владельца.');

  if (noOpen) console.log(`Откройте локальную панель вручную: ${hostUrl}`);
  else openBrowser(hostUrl);

  if (localOnly) {
    console.log('\nЛокальный режим: внешний туннель не запускается.');
    return;
  }
  startCloudflareTunnel(localUrl);
}

async function shutdown() {
  if (shuttingDown) return;
  shuttingDown = true;
  console.log('\nОстанавливаю сессию…');
  if (tunnelProcess && !tunnelProcess.killed) tunnelProcess.kill();
  await app.close();
  process.exit(0);
}

process.once('SIGINT', () => void shutdown());
process.once('SIGTERM', () => void shutdown());

start().catch(async (error) => {
  console.error(`Не удалось запустить сервер: ${error.message}`);
  if (tunnelProcess && !tunnelProcess.killed) tunnelProcess.kill();
  await app.close();
  process.exit(1);
});
