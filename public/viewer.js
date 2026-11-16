const byId = (id) => document.getElementById(id);

const joinForm = byId('join-form');
const codeInput = byId('session-code');
const nameInput = byId('viewer-name');
const joinButton = byId('join-button');
const sessionState = byId('session-state');
const stateTitle = byId('state-title');
const stateCopy = byId('state-copy');
const requestAgainButton = byId('request-again');
const disconnectButton = byId('disconnect-button');
const screenPanel = byId('screen-panel');
const screenLabel = byId('screen-label');
const remoteVideo = byId('remote-video');
const screenPlaceholder = byId('screen-placeholder');
const placeholderTitle = byId('placeholder-title');
const placeholderCopy = byId('placeholder-copy');
const fullscreenButton = byId('fullscreen-button');
const videoFrame = document.querySelector('.video-frame');

let socket = null;
let peerConnection = null;
let pendingCandidates = [];
let sessionCode = '';
let viewerName = 'Гость';
let joined = false;
let approved = false;
let requestPending = false;
let intentionalDisconnect = false;
let shouldRetry = false;

function setState(title, copy, tone = 'waiting', actions = {}) {
  stateTitle.textContent = title;
  stateCopy.textContent = copy;
  sessionState.dataset.tone = tone;
  sessionState.hidden = false;
  requestAgainButton.hidden = !actions.retry;
  disconnectButton.hidden = !actions.disconnect;
}

function formatCodeInput() {
  const raw = codeInput.value.toUpperCase().replace(/[^A-Z0-9]/g, '').slice(0, 10);
  codeInput.value = raw.length > 5 ? `${raw.slice(0, 5)}-${raw.slice(5)}` : raw;
}

function canonicalCode(value) {
  return String(value ?? '').toUpperCase().replace(/[^A-Z0-9]/g, '');
}

function send(message) {
  if (socket?.readyState !== WebSocket.OPEN) return false;
  socket.send(JSON.stringify(message));
  return true;
}

function setPlaceholder(title, copy) {
  placeholderTitle.textContent = title;
  placeholderCopy.textContent = copy;
  screenPlaceholder.hidden = false;
  remoteVideo.hidden = true;
  fullscreenButton.disabled = true;
  screenLabel.textContent = title;
}

function closePeer() {
  if (peerConnection) {
    peerConnection.onicecandidate = null;
    peerConnection.ontrack = null;
    peerConnection.onconnectionstatechange = null;
    peerConnection.oniceconnectionstatechange = null;
    try { peerConnection.close(); } catch { /* already closed */ }
    peerConnection = null;
  }
  pendingCandidates = [];
  remoteVideo.srcObject = null;
  setPlaceholder('Ожидание изображения', 'После разрешения владелец выберет экран для показа.');
}

function ensurePeerConnection() {
  if (peerConnection) return peerConnection;
  peerConnection = new RTCPeerConnection({
    iceServers: [{ urls: 'stun:stun.l.google.com:19302' }],
  });
  peerConnection.onicecandidate = (event) => {
    if (event.candidate) {
      send({ type: 'candidate', payload: event.candidate.toJSON() });
    }
  };
  peerConnection.ontrack = (event) => {
    const stream = event.streams?.[0] ?? new MediaStream([event.track]);
    remoteVideo.srcObject = stream;
    remoteVideo.hidden = false;
    screenPlaceholder.hidden = true;
    fullscreenButton.disabled = false;
    screenLabel.textContent = 'Трансляция активна';
    remoteVideo.play().catch(() => {});
  };
  peerConnection.onconnectionstatechange = () => {
    if (!approved || !peerConnection) return;
    if (peerConnection.connectionState === 'connected') {
      setState('Вы подключены', 'Владелец показывает выбранный экран. Просмотр можно завершить в любой момент.', 'connected', { disconnect: true });
      screenLabel.textContent = 'Трансляция активна';
    } else if (peerConnection.connectionState === 'connecting') {
      setState('Устанавливаем соединение', 'Это может занять несколько секунд.', 'waiting', { disconnect: true });
    } else if (peerConnection.connectionState === 'failed') {
      setState('Не удалось соединиться', 'Сеть может не поддерживать прямое WebRTC-соединение. Попросите владельца завершить и перезапустить сессию.', 'error', { disconnect: true });
    } else if (peerConnection.connectionState === 'disconnected') {
      setState('Связь прервалась', 'Ожидаем восстановления соединения.', 'waiting', { disconnect: true });
    }
  };
  peerConnection.oniceconnectionstatechange = () => {
    if (peerConnection?.iceConnectionState === 'failed') {
      setState('Прямое соединение не установлено', 'В этой версии видео не ретранслируется через сервер. Проверьте сеть или попробуйте другой интернет.', 'error', { disconnect: true });
    }
  };
  return peerConnection;
}

async function addCandidate(candidate) {
  if (!candidate) return;
  if (!peerConnection) {
    pendingCandidates.push(candidate);
    return;
  }
  if (!peerConnection.remoteDescription) {
    pendingCandidates.push(candidate);
    return;
  }
  try { await peerConnection.addIceCandidate(candidate); } catch { /* late candidate after disconnect */ }
}

async function handleOffer(message) {
  if (!approved || !message.payload) return;
  const connection = ensurePeerConnection();
  try {
    await connection.setRemoteDescription(message.payload);
    for (const candidate of pendingCandidates.splice(0)) {
      await connection.addIceCandidate(candidate);
    }
    const answer = await connection.createAnswer();
    await connection.setLocalDescription(answer);
    send({ type: 'answer', payload: connection.localDescription });
    setState('Соединяемся с экраном', 'Поток появится, когда прямое соединение будет готово.', 'waiting', { disconnect: true });
  } catch (error) {
    setState('Ошибка видеосоединения', error.message || 'Не удалось обработать приглашение.', 'error', { disconnect: true });
  }
}

function handleError(message) {
  const title = message.code === 'invalid-code' ? 'Проверьте код' : 'Подключение не выполнено';
  setState(title, message.message || 'Не удалось подключиться к сессии.', 'error', { retry: joined || shouldRetry, disconnect: joined });
  if (!joined) joinButton.disabled = false;
  requestPending = false;
}

function handleMessage(message) {
  switch (message.type) {
    case 'joined':
      joined = true;
      shouldRetry = false;
      joinForm.hidden = true;
      screenPanel.hidden = false;
      screenLabel.textContent = 'Ожидание подтверждения';
      setPlaceholder('Ожидается подтверждение', 'Владелец увидит ваш запрос и сможет принять или отклонить его.');
      break;
    case 'waiting-approval':
      requestPending = true;
      setState('Запрос отправлен', 'Ожидайте: владелец должен подтвердить просмотр лично.', 'waiting', { disconnect: true });
      break;
    case 'approved':
      approved = true;
      requestPending = false;
      ensurePeerConnection();
      setState('Запрос подтверждён', 'Владелец разрешил просмотр. Ожидаем, пока он включит показ экрана.', 'waiting', { disconnect: true });
      setPlaceholder('Ожидание изображения', 'Владелец ещё не включил показ экрана.');
      break;
    case 'sharing-state':
      if (message.active && approved) {
        setState('Владелец включил показ', 'Устанавливаем прямое соединение…', 'waiting', { disconnect: true });
        setPlaceholder('Подключаемся к трансляции', 'Видео появится после установки соединения.');
      } else if (!message.active && approved) {
        closePeer();
        setState('Показ временно выключен', 'Владелец остановил трансляцию. Запросите доступ снова, если хотите продолжить.', 'waiting', { retry: true, disconnect: true });
      }
      break;
    case 'offer':
      void handleOffer(message);
      break;
    case 'candidate':
      void addCandidate(message.payload);
      break;
    case 'permission-revoked':
      approved = false;
      requestPending = false;
      closePeer();
      setState('Доступ остановлен', 'Владелец прекратил показ. Можно попросить разрешение ещё раз.', 'waiting', { retry: true, disconnect: true });
      break;
    case 'denied':
      approved = false;
      requestPending = false;
      setState('Запрос отклонён', 'Владелец не разрешил просмотр. Вы можете отправить новый запрос позже.', 'error', { retry: true });
      shouldRetry = true;
      break;
    case 'host-disconnected':
      approved = false;
      joined = false;
      closePeer();
      screenPanel.hidden = true;
      joinForm.hidden = false;
      joinButton.disabled = false;
      setState('Владелец отключился', 'Попросите владельца запустить новую сессию и передать свежий код.', 'error', { retry: false });
      break;
    case 'error':
      handleError(message);
      break;
    default:
      break;
  }
}

function connectToSession() {
  intentionalDisconnect = false;
  shouldRetry = false;
  joined = false;
  approved = false;
  requestPending = false;
  joinButton.disabled = true;
  sessionState.hidden = false;
  setState('Подключаемся к сессии', 'Проверяем код и отправляем запрос владельцу…', 'waiting');
  const scheme = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  socket = new WebSocket(`${scheme}//${window.location.host}/signal`);

  socket.addEventListener('open', () => {
    send({ type: 'viewer-join', code: sessionCode, name: viewerName });
  });
  socket.addEventListener('message', (event) => {
    let message;
    try { message = JSON.parse(event.data); } catch { return; }
    handleMessage(message);
  });
  socket.addEventListener('error', () => {
    if (!joined) setState('Нет связи с сервером', 'Проверьте ссылку и подключение к интернету.', 'error', { retry: true });
  });
  socket.addEventListener('close', () => {
    if (intentionalDisconnect) return;
    if (joined && shouldRetry) {
      setState('Соединение закрыто', 'Отправьте запрос ещё раз, если владелец готов продолжить.', 'waiting', { retry: true });
      return;
    }
    if (joined) {
      joined = false;
      approved = false;
      closePeer();
      screenPanel.hidden = true;
      joinForm.hidden = false;
      joinButton.disabled = false;
      setState('Соединение прервано', 'Проверьте интернет или попросите владельца запустить сессию снова.', 'error', { retry: true });
    } else {
      joinButton.disabled = false;
    }
  });
}

function retryRequest() {
  if (socket?.readyState === WebSocket.OPEN && joined) {
    if (approved || requestPending) return;
    requestPending = true;
    send({ type: 'request-access' });
    setState('Запрос отправлен', 'Ожидайте решения владельца.', 'waiting', { disconnect: true });
    return;
  }
  if (socket && socket.readyState === WebSocket.CONNECTING) return;
  if (!sessionCode) {
    joinForm.hidden = false;
    joinButton.disabled = false;
    return;
  }
  connectToSession();
}

function disconnect() {
  intentionalDisconnect = true;
  if (socket && socket.readyState < WebSocket.CLOSING) socket.close(1000, 'Viewer left');
  socket = null;
  joined = false;
  approved = false;
  requestPending = false;
  closePeer();
  screenPanel.hidden = true;
  sessionState.hidden = true;
  joinForm.hidden = false;
  joinButton.disabled = false;
}

codeInput.addEventListener('input', formatCodeInput);
joinForm.addEventListener('submit', (event) => {
  event.preventDefault();
  const rawCode = canonicalCode(codeInput.value);
  if (rawCode.length !== 10) {
    setState('Проверьте код', 'Введите все 10 символов кода, который сообщил владелец.', 'error');
    return;
  }
  sessionCode = rawCode;
  viewerName = nameInput.value.trim().slice(0, 32) || 'Гость';
  connectToSession();
});
requestAgainButton.addEventListener('click', retryRequest);
disconnectButton.addEventListener('click', disconnect);
fullscreenButton.addEventListener('click', async () => {
  try {
    if (!document.fullscreenElement) await videoFrame.requestFullscreen();
    else await document.exitFullscreen();
  } catch {
    setState('Полный экран недоступен', 'Попробуйте открыть видео в полноэкранном режиме через меню браузера.', 'error', { disconnect: true });
  }
});

const linkOptions = new URLSearchParams(window.location.hash.slice(1));
const codeFromLink = new URLSearchParams(window.location.search).get('code') || linkOptions.get('code');
const nameFromLink = linkOptions.get('name');
const autoConnect = linkOptions.get('connect') === '1';
if (codeFromLink) {
  codeInput.value = codeFromLink;
  if (nameFromLink) nameInput.value = nameFromLink.slice(0, 32);
  formatCodeInput();
  if (window.location.hash) {
    window.history.replaceState(null, '', `${window.location.pathname}${window.location.search}`);
  }
  if (autoConnect) window.setTimeout(() => joinForm.requestSubmit(), 50);
}
window.setInterval(() => send({ type: 'heartbeat' }), 25_000);
