import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { randomBytes, randomUUID, timingSafeEqual } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { WebSocket, WebSocketServer } from 'ws';

const MODULE_DIR = path.dirname(fileURLToPath(import.meta.url));
const DEFAULT_PUBLIC_DIR = path.resolve(MODULE_DIR, '../public');
const CODE_ALPHABET = '23456789ABCDEFGHJKLMNPQRSTUVWXYZ';
const MAX_MESSAGE_BYTES = 64 * 1024;
const MAX_VIEWERS = 1;

const FILES = new Map([
  ['/', ['index.html', 'text/html; charset=utf-8']],
  ['/app.css', ['app.css', 'text/css; charset=utf-8']],
  ['/viewer.js', ['viewer.js', 'text/javascript; charset=utf-8']],
  ['/host.js', ['host.js', 'text/javascript; charset=utf-8']],
  ['/favicon.svg', ['favicon.svg', 'image/svg+xml']],
]);

export function generateSessionCode() {
  const bytes = randomBytes(10);
  const raw = [...bytes].map((byte) => CODE_ALPHABET[byte % CODE_ALPHABET.length]).join('');
  return `${raw.slice(0, 5)}-${raw.slice(5)}`;
}

function normalizeCode(value) {
  return String(value ?? '').toUpperCase().replace(/[^A-Z0-9]/g, '').slice(0, 10);
}

function constantTimeEqual(value, expected) {
  const left = Buffer.from(String(value ?? ''));
  const right = Buffer.from(String(expected ?? ''));
  return left.length === right.length && left.length > 0 && timingSafeEqual(left, right);
}

function safeSend(client, message) {
  const ws = client?.ws ?? client;
  if (ws?.readyState !== WebSocket.OPEN) return false;
  try {
    ws.send(JSON.stringify(message));
    return true;
  } catch {
    return false;
  }
}

function closeSocket(client, code = 1000, reason = 'Session ended') {
  const ws = client?.ws ?? client;
  if (ws?.readyState === WebSocket.OPEN || ws?.readyState === WebSocket.CONNECTING) {
    try {
      ws.close(code, reason);
    } catch {
      ws.terminate();
    }
  }
}

function jsonResponse(res, status, payload) {
  const body = JSON.stringify(payload);
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    'Cache-Control': 'no-store',
  });
  res.end(body);
}

function textResponse(res, status, message) {
  const body = `${message}\n`;
  res.writeHead(status, {
    'Content-Type': 'text/plain; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    'Cache-Control': 'no-store',
  });
  res.end(body);
}

function isAllowedWebSocketOrigin(request) {
  const origin = request.headers.origin;
  if (typeof origin !== 'string') return false;

  let parsed;
  try {
    parsed = new URL(origin);
  } catch {
    return false;
  }
  if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') return false;

  const allowedHosts = [
    request.headers.host,
    request.headers['x-forwarded-host'],
    request.headers['x-original-host'],
  ]
    .flatMap((value) => String(value ?? '').split(','))
    .map((value) => value.trim().toLowerCase())
    .filter(Boolean);

  if (allowedHosts.includes(parsed.host.toLowerCase())) return true;

  // Quick Tunnels may rewrite Host while forwarding the WebSocket to localhost.
  // Accept their HTTPS browser origin; authentication with the session code or
  // local host key is still required before any signaling data is exchanged.
  return parsed.protocol === 'https:'
    && parsed.hostname.toLowerCase().endsWith('.trycloudflare.com');
}

function isValidRelayPayload(type, payload) {
  if (!payload || typeof payload !== 'object' || Array.isArray(payload)) return false;

  if (type === 'offer' || type === 'answer') {
    return (payload.type === 'offer' || payload.type === 'answer')
      && typeof payload.sdp === 'string'
      && payload.sdp.length <= 60_000;
  }

  if (type === 'candidate') {
    return typeof payload.candidate === 'string' && payload.candidate.length <= 4_096;
  }

  return false;
}

export function createRemoteSupportServer({
  hostKey = randomBytes(32).toString('base64url'),
  sessionCode = generateSessionCode(),
  publicDir = DEFAULT_PUBLIC_DIR,
  maxViewers = MAX_VIEWERS,
} = {}) {
  const expectedCode = normalizeCode(sessionCode);
  const displayCode = `${expectedCode.slice(0, 5)}-${expectedCode.slice(5)}`;
  let publicUrl = null;
  let sharing = false;
  let hostClient = null;
  const viewers = new Map();
  const clients = new Set();
  const failedAttempts = new Map();

  if (expectedCode.length !== 10) {
    throw new Error('sessionCode must contain exactly 10 letters or digits');
  }

  const server = http.createServer(async (request, response) => {
    const headers = {
      'X-Content-Type-Options': 'nosniff',
      'X-Frame-Options': 'DENY',
      'Referrer-Policy': 'no-referrer',
      'Cross-Origin-Resource-Policy': 'same-origin',
      'Permissions-Policy': 'camera=(), microphone=(), geolocation=(), display-capture=(self)',
      'Content-Security-Policy': "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self' ws: wss: stun:; img-src 'self' data:; media-src 'self' blob: mediastream:; object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'",
    };
    for (const [name, value] of Object.entries(headers)) response.setHeader(name, value);

    if (request.method !== 'GET' && request.method !== 'HEAD') {
      response.setHeader('Allow', 'GET, HEAD');
      textResponse(response, 405, 'Method not allowed');
      return;
    }

    let url;
    try {
      url = new URL(request.url ?? '/', `http://${request.headers.host ?? 'localhost'}`);
    } catch {
      textResponse(response, 400, 'Bad request');
      return;
    }

    if (url.pathname === '/api/health') {
      jsonResponse(response, 200, { ok: true });
      return;
    }

    if (url.pathname === '/api/host-info') {
      if (!constantTimeEqual(url.searchParams.get('key'), hostKey)) {
        textResponse(response, 404, 'Not found');
        return;
      }
      jsonResponse(response, 200, {
        sessionCode: displayCode,
        publicUrl,
        sharing,
        viewerConnected: viewers.size > 0,
      });
      return;
    }

    if (url.pathname === '/host') {
      if (!constantTimeEqual(url.searchParams.get('key'), hostKey)) {
        textResponse(response, 404, 'Not found');
        return;
      }
      await serveFile(response, publicDir, 'host.html', 'text/html; charset=utf-8', headers, request.method);
      return;
    }

    const asset = FILES.get(url.pathname);
    if (!asset) {
      textResponse(response, 404, 'Not found');
      return;
    }
    const [filename, contentType] = asset;
    await serveFile(response, publicDir, filename, contentType, headers, request.method);
  });

  const webSocketServer = new WebSocketServer({ noServer: true, maxPayload: MAX_MESSAGE_BYTES });

  server.on('upgrade', (request, socket, head) => {
    let pathname;
    try {
      pathname = new URL(request.url ?? '/', `http://${request.headers.host ?? 'localhost'}`).pathname;
    } catch {
      socket.destroy();
      return;
    }

    if (pathname !== '/signal' || !isAllowedWebSocketOrigin(request)) {
      socket.write('HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n');
      socket.destroy();
      return;
    }

    webSocketServer.handleUpgrade(request, socket, head, (ws) => {
      webSocketServer.emit('connection', ws, request);
    });
  });

  function sendError(client, code, message) {
    safeSend(client, { type: 'error', code, message });
    closeSocket(client, 1008, 'Request rejected');
  }

  function closeViewer(client, code = 1000, reason = 'Session ended') {
    if (client?.role !== 'viewer') return;
    viewers.delete(client.id);
    if (hostClient && hostClient.ws.readyState === WebSocket.OPEN) {
      safeSend(hostClient, { type: 'viewer-left', id: client.id });
    }
    closeSocket(client, code, reason);
  }

  function notifyViewerRequest(viewer) {
    if (!hostClient || hostClient.ws.readyState !== WebSocket.OPEN) return false;
    return safeSend(hostClient, {
      type: 'viewer-request',
      id: viewer.id,
      name: viewer.name,
    });
  }

  function recordFailedAttempt(request) {
    const ip = String(request.headers['cf-connecting-ip'] ?? request.socket.remoteAddress ?? 'unknown');
    const now = Date.now();
    let attempt = failedAttempts.get(ip);
    if (!attempt || now >= attempt.resetAt) {
      attempt = { count: 0, resetAt: now + 60_000 };
      failedAttempts.set(ip, attempt);
    }
    if (attempt.count >= 10) return false;
    attempt.count += 1;
    return true;
  }

  function joinAsViewer(client, message, request) {
    const suppliedCode = normalizeCode(message.code);
    if (!constantTimeEqual(suppliedCode, expectedCode)) {
      const allowed = recordFailedAttempt(request);
      sendError(client, allowed ? 'invalid-code' : 'rate-limited', allowed
        ? 'Код не подошёл. Проверьте его у владельца экрана.'
        : 'Слишком много попыток. Подождите минуту и попробуйте снова.');
      return;
    }

    if (!hostClient || hostClient.ws.readyState !== WebSocket.OPEN) {
      sendError(client, 'host-offline', 'Сессия владельца сейчас недоступна. Попросите его запустить программу ещё раз.');
      return;
    }
    if (viewers.size >= maxViewers) {
      sendError(client, 'room-full', 'К просмотру уже подключён другой участник.');
      return;
    }

    const name = String(message.name ?? '')
      .replace(/[\u0000-\u001f\u007f]/g, '')
      .trim()
      .slice(0, 32) || 'Гость';

    client.role = 'viewer';
    client.id = randomUUID();
    client.name = name;
    client.approved = false;
    client.requestPending = true;
    viewers.set(client.id, client);

    safeSend(client, { type: 'joined', id: client.id });
    safeSend(client, { type: 'waiting-approval' });
    notifyViewerRequest(client);
  }

  function relayFromHost(client, message) {
    const viewer = viewers.get(String(message.id ?? ''));
    if (!viewer?.approved || !isValidRelayPayload(message.type, message.payload)) return;
    if (message.type !== 'offer' && message.type !== 'candidate') return;
    safeSend(viewer, { type: message.type, id: viewer.id, payload: message.payload });
  }

  function relayFromViewer(client, message) {
    if (!client.approved || !hostClient || !isValidRelayPayload(message.type, message.payload)) return;
    if (message.type !== 'answer' && message.type !== 'candidate') return;
    safeSend(hostClient, { type: message.type, id: client.id, payload: message.payload });
  }

  webSocketServer.on('connection', (ws, request) => {
    const client = {
      ws,
      request,
      role: null,
      id: null,
      name: '',
      approved: false,
      requestPending: false,
    };
    clients.add(client);

    ws.on('message', (raw) => {
      let message;
      try {
        message = JSON.parse(raw.toString());
      } catch {
        sendError(client, 'invalid-message', 'Сообщение не распознано.');
        return;
      }
      if (!message || typeof message !== 'object' || Array.isArray(message) || typeof message.type !== 'string') {
        sendError(client, 'invalid-message', 'Сообщение не распознано.');
        return;
      }

      if (!client.role) {
        if (message.type === 'host-auth') {
          if (!constantTimeEqual(message.key, hostKey)) {
            sendError(client, 'unauthorized', 'Не удалось подтвердить локального владельца.');
            return;
          }
          if (hostClient && hostClient.ws.readyState === WebSocket.OPEN) {
            sendError(client, 'host-already-connected', 'Панель владельца уже открыта в другом окне.');
            return;
          }
          client.role = 'host';
          hostClient = client;
          safeSend(client, {
            type: 'host-ready',
            sessionCode: displayCode,
            publicUrl,
            sharing,
          });
          return;
        }
        if (message.type === 'viewer-join') {
          joinAsViewer(client, message, request);
          return;
        }
        sendError(client, 'unauthorized', 'Сначала подтвердите роль участника.');
        return;
      }

      if (message.type === 'heartbeat') {
        safeSend(client, { type: 'heartbeat' });
        return;
      }

      if (client.role === 'host') {
        if (message.type === 'approve') {
          const viewer = viewers.get(String(message.id ?? ''));
          if (!viewer || !viewer.requestPending) return;
          viewer.requestPending = false;
          viewer.approved = true;
          safeSend(viewer, { type: 'approved' });
          safeSend(client, { type: 'viewer-approved', id: viewer.id, name: viewer.name, sharing });
          return;
        }
        if (message.type === 'deny') {
          const viewer = viewers.get(String(message.id ?? ''));
          if (!viewer || !viewer.requestPending) return;
          viewer.requestPending = false;
          safeSend(viewer, { type: 'denied' });
          closeViewer(viewer, 1008, 'Owner declined');
          return;
        }
        if (message.type === 'sharing' && typeof message.active === 'boolean') {
          sharing = message.active;
          for (const viewer of viewers.values()) {
            if (!sharing && viewer.approved) {
              viewer.approved = false;
              safeSend(viewer, { type: 'permission-revoked' });
            }
            safeSend(viewer, { type: 'sharing-state', active: sharing });
          }
          return;
        }
        if (message.type === 'offer' || message.type === 'candidate') {
          relayFromHost(client, message);
        }
        return;
      }

      if (client.role === 'viewer') {
        if (message.type === 'request-access') {
          if (client.approved || client.requestPending) return;
          if (!hostClient || hostClient.ws.readyState !== WebSocket.OPEN) {
            safeSend(client, { type: 'host-disconnected' });
            return;
          }
          client.requestPending = true;
          notifyViewerRequest(client);
          safeSend(client, { type: 'waiting-approval' });
          return;
        }
        if (message.type === 'answer' || message.type === 'candidate') {
          relayFromViewer(client, message);
        }
      }
    });

    ws.on('close', () => {
      clients.delete(client);
      if (client.role === 'host' && hostClient === client) {
        hostClient = null;
        sharing = false;
        for (const viewer of [...viewers.values()]) {
          viewers.delete(viewer.id);
          safeSend(viewer, { type: 'host-disconnected' });
          closeSocket(viewer, 1001, 'Host disconnected');
        }
      } else if (client.role === 'viewer' && viewers.delete(client.id)) {
        if (hostClient && hostClient.ws.readyState === WebSocket.OPEN) {
          safeSend(hostClient, { type: 'viewer-left', id: client.id });
        }
      }
    });

    ws.on('error', () => {});
  });

  return {
    server,
    webSocketServer,
    hostKey,
    sessionCode: displayCode,
    setPublicUrl(value) {
      if (value === null || value === undefined || value === '') {
        publicUrl = null;
        return;
      }
      let parsed;
      try {
        parsed = new URL(value);
      } catch {
        throw new Error('publicUrl must be a valid HTTPS URL');
      }
      if (parsed.protocol !== 'https:' || !parsed.hostname.toLowerCase().endsWith('.trycloudflare.com')) {
        throw new Error('publicUrl must be an HTTPS Cloudflare Quick Tunnel URL');
      }
      publicUrl = parsed.origin;
    },
    async close() {
      for (const client of clients) {
        try {
          client.ws.terminate();
        } catch {
          // The server may already be closing.
        }
      }
      await new Promise((resolve) => {
        if (!server.listening) {
          resolve();
          return;
        }
        server.close(() => resolve());
      });
      await new Promise((resolve) => {
        if (webSocketServer.clients.size === 0) {
          webSocketServer.close(() => resolve());
          return;
        }
        webSocketServer.close(() => resolve());
      });
    },
  };
}

async function serveFile(response, publicDir, filename, contentType, headers, method) {
  let body;
  try {
    body = await readFile(path.join(publicDir, filename));
  } catch {
    textResponse(response, 500, 'Application asset is missing');
    return;
  }

  response.writeHead(200, {
    ...headers,
    'Content-Type': contentType,
    'Content-Length': body.byteLength,
    'Cache-Control': 'no-store',
  });
  response.end(method === 'HEAD' ? undefined : body);
}
