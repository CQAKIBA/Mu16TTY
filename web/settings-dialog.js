const STYLE_ID = "mu16tty-settings-dialog-style";

const DIALOG_CSS = `
.settings-dialog {
  width: min(42rem, calc(100vw - 2rem));
  max-height: calc(100vh - 2rem);
  padding: 0;
  border: 1px solid var(--line, #34437d);
  color: var(--text, #f2f4ff);
  background: var(--panel, #080e2b);
  box-shadow: 0 1rem 4rem #000b;
}
.settings-dialog::backdrop { background: #02070bcc; }
.settings-dialog-fallback {
  position: fixed; inset: 50% auto auto 50%; z-index: 1001;
  transform: translate(-50%, -50%); display: block;
}
.settings-dialog-fallback-backdrop {
  position: fixed; inset: 0; z-index: 1000; background: #02070bcc;
}
.settings-dialog__body { display: grid; gap: .8rem; padding: 1rem; }
.settings-dialog__header { display: flex; align-items: center; justify-content: space-between; gap: 1rem; }
.settings-dialog__title { margin: 0; font-size: 1rem; color: var(--accent, #e43cff); }
.settings-dialog__close { border: 1px solid var(--line, #34437d); background: transparent; color: inherit; padding: .25rem .6rem; }
.settings-dialog__help, .settings-dialog__result, .settings-dialog__input {
  font-family: ui-monospace, "Cascadia Mono", Consolas, "MS Gothic", monospace;
  white-space: pre-wrap;
}
.settings-dialog__help { margin: 0; color: var(--muted, #a9b4d8); font-size: .8rem; }
.settings-dialog__form { display: grid; grid-template-columns: minmax(0, 1fr) auto; gap: .5rem; }
.settings-dialog__input { min-width: 0; border: 1px solid var(--line, #34437d); background: #03061a; color: inherit; padding: .55rem .65rem; }
.settings-dialog__submit { border: 0; background: linear-gradient(90deg, var(--accent, #e43cff), var(--accent-secondary, #238cff)); color: white; font-weight: 700; padding: .55rem 1rem; }
.settings-dialog__result { min-height: 1.3em; margin: 0; color: var(--muted, #a9b4d8); }
.settings-dialog__result[data-kind="success"] { color: var(--green, #75d7a2); }
.settings-dialog__result[data-kind="error"] { color: var(--red, #ff8585); }
@media (max-width: 34rem) {
  .settings-dialog__form { grid-template-columns: 1fr; }
}
`;

function element(tag, className, text = "") {
  const node = document.createElement(tag);
  node.className = className;
  node.textContent = text;
  return node;
}

function ensureStyles() {
  if (document.getElementById(STYLE_ID)) return;
  const style = document.createElement("style");
  style.id = STYLE_ID;
  style.textContent = DIALOG_CSS;
  document.head.append(style);
}

async function responseBody(response) {
  try {
    return await response.json();
  } catch (_error) {
    return { message: `HTTP ${response.status}` };
  }
}

export function createSettingsDialog({ fetchImpl = window.fetch.bind(window), onChanged = async () => {} } = {}) {
  ensureStyles();

  const dialog = document.createElement("dialog");
  dialog.className = "settings-dialog";
  dialog.setAttribute("aria-labelledby", "settings-dialog-title");
  const body = element("section", "settings-dialog__body");
  const header = element("header", "settings-dialog__header");
  const title = element("h2", "settings-dialog__title", "設定コンソール");
  title.id = "settings-dialog-title";
  const closeButton = element("button", "settings-dialog__close", "閉じる");
  closeButton.type = "button";
  const help = element("pre", "settings-dialog__help", "設定情報を取得中…");
  const form = element("form", "settings-dialog__form");
  const input = element("input", "settings-dialog__input");
  input.type = "text";
  input.value = "show";
  input.setAttribute("aria-label", "設定コマンド");
  input.autocomplete = "off";
  const submit = element("button", "settings-dialog__submit", "実行");
  submit.type = "submit";
  const result = element("p", "settings-dialog__result");
  result.setAttribute("aria-live", "polite");
  header.append(title, closeButton);
  form.append(input, submit);
  body.append(header, help, form, result);
  dialog.append(body);
  document.body.append(dialog);

  let opener = null;
  let fallbackBackdrop = null;

  function showResult(message, kind = "") {
    result.textContent = message;
    result.dataset.kind = kind;
  }

  function close() {
    if (typeof dialog.close === "function" && dialog.open && !dialog.classList.contains("settings-dialog-fallback")) {
      dialog.close();
      return;
    }
    dialog.removeAttribute("open");
    dialog.classList.remove("settings-dialog-fallback");
    fallbackBackdrop?.remove();
    fallbackBackdrop = null;
    opener?.focus();
  }

  closeButton.addEventListener("click", close);
  dialog.addEventListener("close", () => opener?.focus());
  dialog.addEventListener("cancel", (event) => {
    event.preventDefault();
    close();
  });
  dialog.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && dialog.classList.contains("settings-dialog-fallback")) {
      event.preventDefault();
      close();
    }
  });

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    submit.disabled = true;
    showResult("実行中…");
    try {
      const response = await fetchImpl("/api/v1/settings/command", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ command: input.value }),
      });
      const payload = await responseBody(response);
      const success = response.ok && payload.success !== false;
      showResult(payload.message || `HTTP ${response.status}`, success ? "success" : "error");
      if (payload.changed) await onChanged();
    } catch (error) {
      showResult(`設定エラー: ${error.message}`, "error");
    } finally {
      submit.disabled = false;
      input.focus();
      input.select();
    }
  });

  async function open(button = document.activeElement) {
    opener = button;
    showResult("");
    help.textContent = "設定情報を取得中…";

    if (typeof dialog.showModal === "function") {
      if (!dialog.open) dialog.showModal();
    } else {
      if (!fallbackBackdrop) {
        fallbackBackdrop = element("div", "settings-dialog-fallback-backdrop");
        document.body.append(fallbackBackdrop);
      }
      dialog.classList.add("settings-dialog-fallback");
      dialog.setAttribute("open", "");
    }
    input.focus();
    input.select();

    try {
      const response = await fetchImpl("/api/v1/settings/help", { cache: "no-store" });
      const payload = await responseBody(response);
      if (!response.ok) throw new Error(payload.message || `HTTP ${response.status}`);
      help.textContent = payload.help;
    } catch (error) {
      help.textContent = `設定ヘルプ取得エラー: ${error.message}`;
      showResult("接続状態を確認して、設定を開き直してください。", "error");
    }
  }

  return { open };
}
