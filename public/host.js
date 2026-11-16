const localKey = new URLSearchParams(window.location.search).get('key');
const byId = (id) => document.getElementById(id);

const connectionBadge = byId('connection-badge');
const connectionText = byId('connection-text');
const tunnelStatus = byId('tunnel-status');
const tunnelText = byId('tunnel-text');
const publicUrlInput = byId('public-url');
const copyLinkButton = byId('copy-link');
const sessionCodeElement = byId('session-code');
const copyCodeButton = byId('copy-code');
const shareIndicator = byId('share-indicator');
const shareStateTitle = byId('share-state-title');
const shareStateCopy = byId('share-state-copy');
const startShareButton = byId('start-share');
const stopShareButton = byId('stop-share');
const localPreviewCard = byId('local-preview-card');
const localPreview = byId('local-preview');
const requestEmpty = byId('request-empty');
const requestPanel = byId('request-panel');
const requestName = byId('request-name');
const approveButton = byId('approve-viewer');
const denyButton = byId('deny-viewer');
const viewerPanel = byId('viewer-panel');
const approvedName = byId('approved-name');
const approvedCopy = byId('approved-copy');
const ownerFeedback = byId('owner-feedback');

let socket = null;
let hostReady = false;
let reconnectTimer = null;
let retryCount = 0;
let fatalSocketError = false;
let shuttingDown = false;
let localStream = null;
let activeRequestId = null;
let approvedViewerId = null;
let approvedViewerName = 'Гость';
const peerConnections = new Map();

function updateBadge(tone, text) {
  connectionText.textContent = text;
  const dot = connectionBadge.querySelector('.status-dot');
  dot.classList.toggle('status-dot-muted', tone === 'muted');
  dot.classList.toggle('status-dot-error', tone === 'error');
}

function setTunnelState(ready, text) {
  tunnelText.textContent = text;
  const dot = tunnelStatus.querySelector('.status-dot');
  dot.classList.toggle('status-dot-muted', !ready);
  dot.classList.toggle('status-dot-error', false);
  copyLinkButton.disabled = !ready || !publicUrlInput.value.startsWith('https://');
}

function showFeedback(text, tone = '') {
  ownerFeedback.textContent = text;
  ownerFeedback.dataset.tone = tone;
  ownerFeedback.hidden = !text;
}

function send(message) {
  if (socket?.readyState !== WebSocket.OPEN) return false;
  socket.send(JSON.stringify(message));
  return true;
}

async function loadHostInfo() {
  if (!localKey) {
    updateBadge('error', 'Нет ключа локальной панели');
    showFeedback('Откройте адрес панели владельца, который программа вывела в терминал. Не пересылайте его другим.', 'error');
    return;
  }

  try {
    const response = await fetch(`/api/host-info?key=${encodeURIComponent(localKey)}`, { cache: 'no-store' });
    if (!response.ok) throw new Error('Ключ панели не подтверждён. Перезапустите программу и откройте её локальный адрес.');
    const info = await response.json();

    sessionCodeElement.textContent = info.sessionCode;
    copyCodeButton.disabled = false;
    if (info.publicUrl) {
      publicUrlInput.value = info.publicUrl;
      setTunnelState(true, 'Временный туннель готов');
    } else {
      publicUrlInput.value = 'Ожидаем Cloudflare Tunnel…';
      setTunnelState(false, 'Ожидаем Cloudflare Tunnel');
    }
  } catch (error) {
    if (publicUrlInput.value.startsWith('Ожидаем')) {
      setTunnelState(false, 'Локальная сессия');
    }
    showFeedback(error.message || 'Не удалось получить данные сессии.', 'error');
  }
}

function resetRequestPanel() {
  activeRequestId = null;
  requestPanel.hidden = true;
  requestEmpty.hidden = false;
}

function closePeer(viewerId) {
  const record = peerConnections.get(viewerId);
  if (!record) return;
  peerConnections.delete(viewerId);
  record.pc.onicecandidate = null;
  record.pc.onconnectionstatechange = null;
  record.pc.oniceconnectionstatechange = null;
  try { record.pc.close(); } catch { /* already closed */ }
}

function closeAllPeers() {
  for (const id of [...peerConnections.keys()]) closePeer(id);
}

function updateApprovedStatus(text) {
  if (approvedViewerId) {
    viewerPanel.hidden = false;
    approvedName.textContent = approvedViewerName;
    approvedCopy.textContent = text;
  }
}

function handleViewerRequest(message) {
  activeRequestId = message.id;
  approveButton.disabled = false;
  denyButton.disabled = false;
  requestName.textContent = message.name || 'Гость';
  requestPanel.hidden = false;
  requestEmpty.hidden = true;
  showFeedback('Новый запрос получен. Разрешите просмотр только если вы знаете этого человека.', '');
}

async function createPeerConnection(viewerId) {
  if (!localStream || !hostReady || viewerId !== approvedViewerId || peerConnections.has(viewerId)) return;

  const connection = new RTCPeerConnection({
    iceServers: [{ urls: 'stun:stun.l.google.com:19302' }],
  });
  const record = { pc: connection, pendingCandidates: [] };
  peerConnections.set(viewerId, record);

  for (const track of localStream.getTracks()) connection.addTrack(track, localStream);
  connection.onicecandidate = (event) => {
    if (event.candidate) {
      send({ type: 'candidate', id: viewerId, payload: event.candidate.toJSON() });
    }
  };
  connection.onconnectionstatechange = () => {
    if (viewerId !== approvedViewerId) return;
    if (connection.connectionState === 'connected') {
      updateApprovedStatus('Гость подключён и видит выбранный экран.');
    } else if (connection.connectionState === 'connecting') {
      updateApprovedStatus('Устанавливаем прямое соединение…');
    } else if (connection.connectionState === 'failed') {
      updateApprovedStatus('Прямое соединение не удалось. Сеть может требовать TURN-сервер.');
    } else if (connection.connectionState === 'disconnected') {
      updateApprovedStatus('Связь прервалась. Ожидаем повторное подключение…');
    }
  };

  try {
    const offer = await connection.createOffer();
    await connection.setLocalDescription(offer);
    if (!send({ type: 'offer', id: viewerId, payload: connection.localDescription })) {
      updateApprovedStatus('Нет связи с сервером сигнализации.');
    } else {
      updateApprovedStatus('Приглашение принято. Ожидаем соединение с гостем…');
    }
  } catch (error) {
    closePeer(viewerId);
    showFeedback(`Не удалось создать видеосоединение: ${error.message}`, 'error');
  }
}

async function handleAnswer(message) {
  const record = peerConnections.get(message.id);
  if (!record || !message.payload) return;
  try {
    await record.pc.setRemoteDescription(message.payload);
    for (const candidate of record.pendingCandidates.splice(0)) {
      await record.pc.addIceCandidate(candidate);
    }
  } catch (error) {
    showFeedback(`Ошибка установки соединения: ${error.message}`, 'error');
  }
}

async function handleCandidate(message) {
  const record = peerConnections.get(message.id);
  if (!record || !message.payload) return;
  try {
    if (record.pc.remoteDescription) await record.pc.addIceCandidate(message.payload);
    else record.pendingCandidates.push(message.payload);
  } catch {
    // A stale ICE candidate can arrive while a viewer is disconnecting.
  }
}

function handleMessage(message) {
  switch (message.type) {
    case 'host-ready':
      hostReady = true;
      retryCount = 0;
      updateBadge('ready', 'Панель подключена');
      startShareButton.disabled = false;
      sessionCodeElement.textContent = message.sessionCode;
      showFeedback('Сессия активна. Передайте гостю публичную ссылку и код доступа.', 'success');
      break;
    case 'viewer-request':
      handleViewerRequest(message);
      break;
    case 'viewer-approved':
      activeRequestId = null;
      requestPanel.hidden = true;
      requestEmpty.hidden = true;
      approvedViewerId = message.id;
      approvedViewerName = message.name || 'Гость';
      viewerPanel.hidden = false;
      approvedName.textContent = approvedViewerName;
      if (localStream) {
        updateApprovedStatus('Разрешение выдано. Подключаем гостя к экрану…');
        void createPeerConnection(approvedViewerId);
      } else {
        updateApprovedStatus('Разрешение выдано. Включите показ экрана, чтобы гость увидел изображение.');
      }
      showFeedback(`Вы разрешили просмотр для ${approvedViewerName}. Остановите показ в любой момент.`, 'success');
      break;
    case 'viewer-left':
      closePeer(message.id);
      if (message.id === activeRequestId) resetRequestPanel();
      if (message.id === approvedViewerId) {
        approvedViewerId = null;
        viewerPanel.hidden = true;
        requestEmpty.hidden = false;
        showFeedback('Гость отключился.', '');
      }
      break;
    case 'offer':
      break;
    case 'answer':
      void handleAnswer(message);
      break;
    case 'candidate':
      void handleCandidate(message);
      break;
    case 'error':
      updateBadge('error', message.message || 'Ошибка соединения');
      if (message.code === 'host-already-connected' || message.code === 'unauthorized') {
        fatalSocketError = true;
        startShareButton.disabled = true;
      }
      showFeedback(message.message || 'Сервер отклонил запрос.', 'error');
      break;
    default:
      break;
  }
}

function connectSocket() {
  if (shuttingDown || fatalSocketError || !localKey) return;
  const scheme = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  updateBadge('muted', 'Подключаем панель…');
  socket = new WebSocket(`${scheme}//${window.location.host}/signal`);

  socket.addEventListener('open', () => {
    send({ type: 'host-auth', key: localKey });
  });
  socket.addEventListener('message', (event) => {
    let message;
    try { message = JSON.parse(event.data); } catch { return; }
    handleMessage(message);
  });
  socket.addEventListener('error', () => {
    updateBadge('error', 'Проблема с подключением');
  });
  socket.addEventListener('close', () => {
    hostReady = false;
    startShareButton.disabled = true;
    updateBadge('error', 'Связь потеряна');
    activeRequestId = null;
    requestPanel.hidden = true;
    requestEmpty.hidden = false;
    approvedViewerId = null;
    viewerPanel.hidden = true;
    if (localStream) void stopSharing(false);
    closeAllPeers();
    if (!shuttingDown && !fatalSocketError) {
      const wait = Math.min(1000 * (2 ** Math.min(retryCount, 4)), 15_000);
      retryCount += 1;
      reconnectTimer = window.setTimeout(connectSocket, wait);
    }
  });
}

async function startSharing() {
  if (!hostReady || localStream) return;
  if (!navigator.mediaDevices?.getDisplayMedia) {
    showFeedback('Этот браузер не поддерживает показ экрана. Откройте панель в актуальном Chrome, Edge или Firefox.', 'error');
    return;
  }

  startShareButton.disabled = true;
  showFeedback('Выберите экран или окно в системном диалоге браузера. До выбора ничего не передаётся.', '');
  try {
    const stream = await navigator.mediaDevices.getDisplayMedia({
      video: {
        width: { ideal: 1280, max: 1920 },
        height: { ideal: 720, max: 1080 },
        frameRate: { ideal: 15, max: 20 },
      },
      audio: false,
    });
    localStream = stream;
    localPreview.srcObject = stream;
    localPreviewCard.hidden = false;
    shareIndicator.classList.add('is-live');
    shareStateTitle.textContent = 'Показ включён';
    shareStateCopy.textContent = 'Транслируется только выбранное вами окно или экран.';
    startShareButton.hidden = true;
    stopShareButton.hidden = false;
    send({ type: 'sharing', active: true });
    const videoTrack = stream.getVideoTracks()[0];
    videoTrack?.addEventListener('ended', () => void stopSharing(true), { once: true });
    showFeedback('Показ включён. Чтобы завершить трансляцию, нажмите «Остановить показ» или завершите её в панели браузера.', 'success');
    if (approvedViewerId) void createPeerConnection(approvedViewerId);
  } catch (error) {
    startShareButton.disabled = !hostReady;
    if (error.name === 'NotAllowedError') {
      showFeedback('Вы отменили выбор экрана. Ничего не передавалось.', '');
    } else {
      showFeedback(`Не удалось начать показ экрана: ${error.message}`, 'error');
    }
  }
}

async function stopSharing(notifyServer = true) {
  const stream = localStream;
  if (!stream) return;
  localStream = null;
  closeAllPeers();
  if (notifyServer) send({ type: 'sharing', active: false });
  for (const track of stream.getTracks()) {
    try { track.stop(); } catch { /* already stopped by the browser */ }
  }
  localPreview.srcObject = null;
  localPreviewCard.hidden = true;
  shareIndicator.classList.remove('is-live');
  shareStateTitle.textContent = 'Показ выключен';
  shareStateCopy.textContent = 'Чтобы начать снова, выберите экран в системном диалоге.';
  startShareButton.hidden = false;
  startShareButton.disabled = !hostReady;
  stopShareButton.hidden = true;
  if (approvedViewerId) {
    approvedViewerId = null;
    viewerPanel.hidden = true;
    requestEmpty.hidden = false;
  }
  showFeedback('Показ экрана остановлен. Для новой трансляции нужно снова разрешить гостю доступ.', '');
}

async function copyText(value, successMessage) {
  if (!value) return;
  try {
    await navigator.clipboard.writeText(value);
    showFeedback(successMessage, 'success');
  } catch {
    const temporary = document.createElement('textarea');
    temporary.value = value;
    temporary.setAttribute('readonly', '');
    temporary.className = 'clipboard-fallback';
    document.body.append(temporary);
    temporary.select();
    const copied = document.execCommand('copy');
    temporary.remove();
    showFeedback(copied ? successMessage : 'Не удалось скопировать. Выделите значение и скопируйте вручную.', copied ? 'success' : 'error');
  }
}

startShareButton.addEventListener('click', () => void startSharing());
stopShareButton.addEventListener('click', () => void stopSharing(true));
approveButton.addEventListener('click', () => {
  if (!activeRequestId || !send({ type: 'approve', id: activeRequestId })) {
    showFeedback('Нет соединения с сервером. Запрос пока не подтверждён.', 'error');
    return;
  }
  approveButton.disabled = true;
  denyButton.disabled = true;
});
denyButton.addEventListener('click', () => {
  if (!activeRequestId) return;
  send({ type: 'deny', id: activeRequestId });
  resetRequestPanel();
  showFeedback('Запрос отклонён. Гость не получит изображение.', '');
});
copyLinkButton.addEventListener('click', () => copyText(publicUrlInput.value, 'Ссылка скопирована. Отправьте её только нужному человеку.'));
copyCodeButton.addEventListener('click', () => copyText(sessionCodeElement.textContent, 'Код скопирован. Отправьте его отдельно от ссылки.'));

window.addEventListener('pagehide', () => {
  shuttingDown = true;
  window.clearTimeout(reconnectTimer);
  send({ type: 'sharing', active: false });
  closeAllPeers();
  for (const track of localStream?.getTracks() ?? []) {
    try { track.stop(); } catch { /* already stopped */ }
  }
  socket?.close();
});

void loadHostInfo();
window.setInterval(loadHostInfo, 2500);
window.setInterval(() => send({ type: 'heartbeat' }), 25_000);
connectSocket();
