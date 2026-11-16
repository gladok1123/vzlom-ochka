import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const { createViewerUrl, parseTunnelUrl } = require('../desktop/connection-utils.cjs');

test('builds a viewer URL without putting the code in the HTTP query', () => {
  const session = createViewerUrl(
    'https://example-room.trycloudflare.com/',
    'k7m2q-r9c4d',
    '  Алексей\n  ',
  );
  const target = new URL(session.url);
  const fragment = new URLSearchParams(target.hash.slice(1));

  assert.equal(session.origin, 'https://example-room.trycloudflare.com');
  assert.equal(target.search, '');
  assert.equal(target.hash.includes('K7M2Q-R9C4D'), false);
  assert.equal(fragment.get('code'), 'K7M2QR9C4D');
  assert.equal(fragment.get('connect'), '1');
  assert.equal(fragment.get('name'), 'Алексей');
});

test('accepts only a root HTTPS Cloudflare Quick Tunnel URL', () => {
  assert.equal(parseTunnelUrl('https://example-room.trycloudflare.com'), 'https://example-room.trycloudflare.com');
  assert.throws(() => parseTunnelUrl('http://example-room.trycloudflare.com'));
  assert.throws(() => parseTunnelUrl('https://example.com'));
  assert.throws(() => parseTunnelUrl('https://user@example-room.trycloudflare.com'));
  assert.throws(() => parseTunnelUrl('https://example-room.trycloudflare.com/private'));
  assert.throws(() => parseTunnelUrl('https://example-room.trycloudflare.com/?code=secret'));
});

test('rejects malformed codes and removes control characters from the viewer name', () => {
  assert.throws(() => createViewerUrl('https://example-room.trycloudflare.com', '12345'));
  assert.throws(() => createViewerUrl('https://example-room.trycloudflare.com', 'ABCDE-1G234'));
  const session = createViewerUrl('https://example-room.trycloudflare.com', 'K7M2Q-R9C4D', 'Guest\u0000'.padEnd(40, 'x'));
  assert.equal(session.name.length, 32);
  assert.equal(session.name.includes('\u0000'), false);
});
