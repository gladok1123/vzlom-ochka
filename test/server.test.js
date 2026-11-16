import test from 'node:test';
import assert from 'node:assert/strict';
import { WebSocket } from 'ws';
import { createRemoteSupportServer } from '../src/server.js';

async function startTestServer() {
  const app = createRemoteSupportServer({
    hostKey: 'local-test-owner-key',
    sessionCode: 'ABCDE-FG234',
  });
  await new Promise((resolve, reject) => {
    app.server.once('error', reject);
    app.server.listen(0, '127.0.0.1', resolve);
  });
  const address = app.server.address();
  const baseUrl = `http://127.0.0.1:${address.port}`;
  return { app, baseUrl, wsUrl: `ws://127.0.0.1:${address.port}/signal` };
}

function openWebSocket(url, origin) {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(url, { headers: { Origin: origin } });
    socket.once('open', () => resolve(socket));
    socket.once('error', reject);
  });
}

function waitForMessage(socket, predicate, timeout = 1_500) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      socket.off('message', onMessage);
      reject(new Error('Timed out waiting for a WebSocket message'));
    }, timeout);
    const onMessage = (data) => {
      let message;
      try { message = JSON.parse(data.toString()); } catch { return; }
      if (!predicate(message)) return;
      clearTimeout(timer);
      socket.off('message', onMessage);
      resolve(message);
    };
    socket.on('message', onMessage);
  });
}

test('serves the viewer UI without exposing the session code', async (t) => {
  const { app, baseUrl } = await startTestServer();
  t.after(() => app.close());

  const response = await fetch(`${baseUrl}/`);
  const body = await response.text();
  assert.equal(response.status, 200);
  assert.match(response.headers.get('content-security-policy'), /frame-ancestors 'none'/);
  assert.match(response.headers.get('x-content-type-options'), /nosniff/);
  assert.match(body, /Подключиться к сессии/);
  assert.doesNotMatch(body, /ABCDE-FG234/);

  const unauthorized = await fetch(`${baseUrl}/api/host-info?key=wrong`);
  assert.equal(unauthorized.status, 404);
  const authorized = await fetch(`${baseUrl}/api/host-info?key=local-test-owner-key`);
  assert.equal(authorized.status, 200);
  assert.deepEqual(await authorized.json(), {
    sessionCode: 'ABCDE-FG234',
    publicUrl: null,
    sharing: false,
    viewerConnected: false,
  });
});

test('only an approved viewer receives signaling data', async (t) => {
  const { app, baseUrl, wsUrl } = await startTestServer();
  const sockets = [];
  t.after(async () => {
    for (const socket of sockets) socket.terminate();
    await app.close();
  });

  const host = await openWebSocket(wsUrl, baseUrl);
  sockets.push(host);
  const hostReady = waitForMessage(host, (message) => message.type === 'host-ready');
  host.send(JSON.stringify({ type: 'host-auth', key: 'local-test-owner-key' }));
  await hostReady;

  const viewer = await openWebSocket(wsUrl, baseUrl);
  sockets.push(viewer);
  const joined = waitForMessage(viewer, (message) => message.type === 'joined');
  const waiting = waitForMessage(viewer, (message) => message.type === 'waiting-approval');
  const request = waitForMessage(host, (message) => message.type === 'viewer-request');
  viewer.send(JSON.stringify({ type: 'viewer-join', code: 'ABCDE-FG234', name: 'Тестовый гость' }));
  await Promise.all([joined, waiting]);
  const viewerRequest = await request;

  const offerBeforeApproval = waitForMessage(viewer, () => true, 100).catch(() => null);
  host.send(JSON.stringify({
    type: 'offer',
    id: viewerRequest.id,
    payload: { type: 'offer', sdp: 'v=0\r\n' },
  }));
  assert.equal(await offerBeforeApproval, null);

  const approved = waitForMessage(viewer, (message) => message.type === 'approved');
  const hostApproved = waitForMessage(host, (message) => message.type === 'viewer-approved');
  host.send(JSON.stringify({ type: 'approve', id: viewerRequest.id }));
  await Promise.all([approved, hostApproved]);

  const relayedOffer = waitForMessage(viewer, (message) => message.type === 'offer');
  host.send(JSON.stringify({
    type: 'offer',
    id: viewerRequest.id,
    payload: { type: 'offer', sdp: 'v=0\r\n' },
  }));
  assert.equal((await relayedOffer).payload.sdp, 'v=0\r\n');

  const relayedAnswer = waitForMessage(host, (message) => message.type === 'answer');
  viewer.send(JSON.stringify({
    type: 'answer',
    payload: { type: 'answer', sdp: 'v=0\r\n' },
  }));
  assert.equal((await relayedAnswer).id, viewerRequest.id);
});

test('rejects WebSocket connections from an unrelated browser origin', async (t) => {
  const { app, wsUrl } = await startTestServer();
  t.after(() => app.close());

  const socket = new WebSocket(wsUrl, { headers: { Origin: 'https://attacker.example' } });
  const close = await new Promise((resolve, reject) => {
    socket.once('close', (code) => resolve(code));
    socket.once('open', () => reject(new Error('Unexpectedly accepted a foreign Origin')));
    socket.once('error', () => resolve(1006));
  });
  assert.ok(close === 1006 || close === 1002);
});
