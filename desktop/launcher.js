const connectForm = document.getElementById('connect-form');
const urlInput = document.getElementById('tunnel-url');
const codeInput = document.getElementById('session-code');
const nameInput = document.getElementById('viewer-name');
const connectButton = document.getElementById('connect-button');
const connectState = document.getElementById('connect-state');
const stateTitle = document.getElementById('state-title');
const stateCopy = document.getElementById('state-copy');

function showState(title, message, tone = 'waiting') {
  stateTitle.textContent = title;
  stateCopy.textContent = message;
  connectState.dataset.tone = tone;
  connectState.hidden = false;
}

function formatCode() {
  const raw = codeInput.value.toUpperCase().replace(/[^A-Z0-9]/g, '').slice(0, 10);
  codeInput.value = raw.length > 5 ? `${raw.slice(0, 5)}-${raw.slice(5)}` : raw;
}

codeInput.addEventListener('input', formatCode);
connectForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  const url = urlInput.value.trim();
  const code = codeInput.value.trim();
  if (!url || !code) {
    showState('Заполните приглашение', 'Нужны публичная ссылка и код от владельца Linux.', 'error');
    return;
  }
  if (!window.screenBridge?.connectSession) {
    showState('Приложение не готово', 'Перезапустите программу и попробуйте ещё раз.', 'error');
    return;
  }

  connectButton.disabled = true;
  showState('Открываем приглашение', 'Проверяем адрес и запускаем подключение…');
  try {
    const name = nameInput.value.trim().slice(0, 32);
    const result = await window.screenBridge.connectSession({ url, code, name });
    if (!result?.ok) {
      connectButton.disabled = false;
      showState('Не удалось подключиться', result?.error || 'Проверьте ссылку и код.', 'error');
      return;
    }
    showState('Запрос отправлен', 'Страница просмотра откроется сейчас. Владелец Linux должен принять запрос.', 'success');
  } catch {
    connectButton.disabled = false;
    showState('Не удалось открыть сессию', 'Проверьте ссылку и подключение к интернету.', 'error');
  }
});

window.screenBridge?.onConnectFailed((message) => {
  connectButton.disabled = false;
  showState('Ссылка не открылась', message, 'error');
});
