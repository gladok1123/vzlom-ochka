const fs = require('node:fs');
const path = require('node:path');

const desktopDir = path.resolve(__dirname, '..');
const repositoryDir = path.resolve(desktopDir, '..');
const assetsDir = path.join(desktopDir, 'assets');

fs.mkdirSync(assetsDir, { recursive: true });
for (const filename of ['app.css', 'favicon.svg']) {
  fs.copyFileSync(
    path.join(repositoryDir, 'public', filename),
    path.join(assetsDir, filename),
  );
}
