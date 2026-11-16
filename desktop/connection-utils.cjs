const CODE_ALPHABET = '23456789ABCDEFGHJKLMNPQRSTUVWXYZ';

function normalizeCode(value) {
  return String(value ?? '').toUpperCase().replace(/[^A-Z0-9]/g, '');
}

function parseTunnelUrl(value) {
  let url;
  try {
    url = new URL(String(value ?? '').trim());
  } catch {
    throw new Error('Вставьте полную ссылку Cloudflare, например https://abc-def.trycloudflare.com.');
  }

  if (url.protocol !== 'https:'
      || !url.hostname.toLowerCase().endsWith('.trycloudflare.com')
      || url.username
      || url.password
      || (url.port && url.port !== '443')
      || (url.pathname !== '/' && url.pathname !== '')
      || url.search
      || url.hash) {
    throw new Error('Поддерживается только корневая HTTPS-ссылка вида https://….trycloudflare.com.');
  }
  return url.origin;
}

function createViewerUrl(urlValue, codeValue, nameValue = '') {
  const origin = parseTunnelUrl(urlValue);
  const code = normalizeCode(codeValue);
  if (code.length !== 10 || [...code].some((letter) => !CODE_ALPHABET.includes(letter))) {
    throw new Error('Проверьте код: в нём должно быть 10 символов.');
  }

  const name = String(nameValue ?? '')
    .replace(/[\u0000-\u001f\u007f]/g, '')
    .trim()
    .slice(0, 32);
  const target = new URL(origin);
  const options = new URLSearchParams({ code, connect: '1' });
  if (name) options.set('name', name);
  target.hash = options.toString();
  return { origin, url: target.toString(), code, name };
}

module.exports = { createViewerUrl, normalizeCode, parseTunnelUrl };
