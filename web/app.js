import {
  createState,
  formatTransmitEstimate,
  messageDisplayFields,
  qualitySummary,
  splitMessageIcon,
  normalizeTimelineView,
  reduceAccepted,
  reduceEvent,
  reduceHistory,
  reduceStatus,
  utf8Length,
} from "/assets/state.js";
import {
  acceptMonitorSnapshot,
  appendWaterfall,
  decoderDisplayRows,
  drawWaveform,
} from "/assets/monitor.js";
import { createSettingsDialog } from "/assets/settings-dialog.js";

const elements = Object.fromEntries(
  [
    "callsign",
    "connection-status",
    "service-state",
    "rx-state",
    "tx-state",
    "ptt-state",
    "detail-list",
    "notice",
    "timeline",
    "empty-state",
    "new-messages",
    "timeline-view-list",
    "timeline-view-messenger",
    "message-detail",
    "message-detail-toggle",
    "receive-preview",
    "receive-preview-toggle",
    "composer-form",
    "message-input",
    "byte-count",
    "mode-40",
    "mode-20",
    "mode-10",
    "mode-4",
    "send-button",
    "composer-status",
    "monitor-toggle",
    "monitor-close",
    "monitor-backdrop",
    "monitor-state",
    "monitor-rms",
    "monitor-peak",
    "monitor-level",
    "input-queue",
    "input-queue-count",
    "monitor-waveform",
    "monitor-waterfall",
    "decoder-status-list",
    "decoder-log",
    "settings-button",
  ].map((id) => [id, document.getElementById(id)]),
);

let state = createState();
let connection = "connecting";
let submitPending = false;
let timelineView = readTimelineView();
let selectedKey = null;
let historyCursor = 0;
let historyGeneration = null;
let historyRevision = null;
let refreshRecentHistory = false;

function selectedSymbolRate() {
  const selected = document.querySelector('input[name="mode"]:checked');
  return Number(selected?.value ?? 40);
}
const readKeys = new Set();
let monitorRevision = 0;

function readTimelineView() {
  try {
    return normalizeTimelineView(localStorage.getItem("mu16tty.timelineView"));
  } catch {
    return "list";
  }
}

function setTimelineView(view, persist = true) {
  timelineView = normalizeTimelineView(view);
  const shell = document.getElementById("app-shell");
  shell.classList.toggle("timeline-list", timelineView === "list");
  shell.classList.toggle("timeline-messenger", timelineView === "messenger");
  elements.timeline.dataset.view = timelineView;
  elements["timeline-view-list"].setAttribute("aria-pressed", String(timelineView === "list"));
  elements["timeline-view-messenger"].setAttribute(
    "aria-pressed",
    String(timelineView === "messenger"),
  );
  if (persist) {
    try {
      localStorage.setItem("mu16tty.timelineView", timelineView);
    } catch {
      // Private browsing or disabled storage must not block the modem UI.
    }
  }
}

function setMonitorOpen(open) {
  const shell = document.getElementById("app-shell");
  shell.classList.toggle("monitor-open", open);
  elements["monitor-toggle"].setAttribute("aria-expanded", String(open));
  elements["monitor-backdrop"].hidden = !open;
}

function formatDbfs(value) {
  return Number.isFinite(value) ? value.toFixed(1) : "—";
}

function setMonitorState(label, className) {
  setText(elements["monitor-state"], label);
  elements["monitor-state"].className = `monitor-status ${className}`;
}

function renderDecoderRows(decoders) {
  const fragment = document.createDocumentFragment();
  for (const row of decoderDisplayRows(decoders)) {
    const item = document.createElement("div");
    item.className = `decoder-row decoder-${row.phase}`;
    const lamp = document.createElement("span");
    lamp.className = "decoder-lamp";
    lamp.setAttribute("aria-hidden", "true");
    const name = document.createElement("strong");
    name.textContent = row.name;
    const label = document.createElement("span");
    label.textContent = row.label;
    const elapsed = document.createElement("time");
    elapsed.textContent = row.elapsed;
    item.append(lamp, name, label, elapsed);
    fragment.append(item);
  }
  elements["decoder-status-list"].replaceChildren(fragment);
}

function renderDecoderLog(entries) {
  const lines = (Array.isArray(entries) ? entries : []).map((entry) => {
    const time = Number.isFinite(Number(entry.at))
      ? new Date(Number(entry.at) * 1000).toLocaleTimeString("ja-JP", { hour12: false })
      : "--:--:--";
    const detail = entry.detail ? ` ${entry.detail}` : "";
    return `${time} ${entry.name ?? `Decoder ${entry.symbol_rate ?? ""}`} ${String(entry.phase ?? "").toUpperCase()}${detail}`;
  });
  const value = lines.join("\n");
  if (elements["decoder-log"].value !== value) {
    elements["decoder-log"].value = value;
    elements["decoder-log"].scrollTop = elements["decoder-log"].scrollHeight;
  }
}

async function refreshMonitor() {
  if (document.hidden) return;
  try {
    const response = await fetch("/api/v1/monitor", { cache: "no-store" });
    if (!response.ok) throw new Error(`monitor ${response.status}`);
    const payload = await response.json();
    const queue = payload.input_queue;
    const meter = elements["input-queue"];
    meter.hidden = !queue?.capacity;
    if (!meter.hidden) {
      meter.style.setProperty("--queue-used", `${100 * queue.used / queue.capacity}%`);
      meter.style.setProperty("--queue-peak", `${100 * queue.peak / queue.capacity}%`);
      setText(elements["input-queue-count"], `${queue.used}/${queue.capacity} · peak ${queue.peak}`);
    }
    renderReceivePreviews(payload.receive_previews);
    renderDecoderRows(payload.decoders);
    renderDecoderLog(payload.decoder_log);
    if (!payload.available) {
      setMonitorState("NO INPUT", "monitor-idle");
      return;
    }
    if (payload.clipped) setMonitorState("CLIPPED", "monitor-clipped");
    else setMonitorState("RX ACTIVE", "monitor-active");
    setText(elements["monitor-rms"], `RMS ${formatDbfs(payload.rms_dbfs)} dBFS`);
    setText(elements["monitor-peak"], `Peak ${formatDbfs(payload.peak_dbfs)} dBFS`);
    const level = Math.max(0, Math.min(100, ((payload.rms_dbfs ?? -120) + 60) * (100 / 60)));
    elements["monitor-level"].style.width = `${level}%`;
    const accepted = acceptMonitorSnapshot(monitorRevision, payload);
    if (!accepted.accepted) return;
    monitorRevision = accepted.revision;
    drawWaveform(elements["monitor-waveform"], payload.waveform, payload.clipped);
    appendWaterfall(elements["monitor-waterfall"], payload.spectrum_dbfs);
  } catch {
    elements["input-queue"].hidden = true;
    setMonitorState("OFFLINE", "monitor-offline");
  }
}

let receivePreviewKey = "";
let receivePreviewActive = false;
let receivePreviewPinned = false;
let receivePreviewDismissed = false;

function updateReceivePreviewVisibility() {
  const open = receivePreviewPinned || (receivePreviewActive && !receivePreviewDismissed);
  const panel = elements["receive-preview"];
  panel.hidden = !open;
  if (open) panel.scrollTop = panel.scrollHeight;
  const toggle = elements["receive-preview-toggle"];
  toggle.setAttribute("aria-expanded", String(open));
  setText(toggle, open ? "閉じる" : "開く");
}

elements["receive-preview-toggle"].addEventListener("click", () => {
  receivePreviewPinned = elements["receive-preview"].hidden;
  receivePreviewDismissed = !receivePreviewPinned && receivePreviewActive;
  updateReceivePreviewVisibility();
});

function renderReceivePreviews(previews = []) {
  const key = JSON.stringify(previews);
  if (key === receivePreviewKey) return;
  receivePreviewKey = key;
  receivePreviewActive = previews.length > 0;
  if (!receivePreviewActive) receivePreviewDismissed = false;
  const panel = elements["receive-preview"];
  panel.replaceChildren();
  if (!receivePreviewActive) addText(panel, "receive-preview-label", "受信待機中");
  for (const preview of previews) {
    addText(panel, "receive-preview-label", `受信中・暫定 · ${preview.symbol_rate}/s`);
    addText(panel, "receive-preview-text", preview.text);
  }
  updateReceivePreviewVisibility();
}

function setText(element, value) {
  element.textContent = value ?? "";
}

function chip(element, label, value) {
  setText(element, `${label} ${value ?? "--"}`);
  element.className = "chip";
  if (["running", "live"].includes(value)) element.classList.add("chip-live");
  else if (["degraded", "retrying", "starting"].includes(value)) element.classList.add("chip-warn");
  else if (["failed", "offline"].includes(value)) element.classList.add("chip-error");
  else element.classList.add("chip-muted");
}

function nearTimelineBottom() {
  const { scrollTop, scrollHeight, clientHeight } = elements.timeline;
  return scrollHeight - scrollTop - clientHeight < 80;
}

function deliveryLabel(item) {
  const labels = {
    queued: "送信待ち",
    sent: "送信済み",
    loopback_received: "ループバック受信",
    failed: `失敗 ${item.errorCode ?? ""}`.trim(),
    received: "受信",
  };
  return labels[item.delivery] ?? item.delivery;
}

function addText(parent, className, text) {
  const element = document.createElement("span");
  element.className = className;
  setText(element, text);
  parent.append(element);
  return element;
}

function messageElement(item) {
  const fields = messageDisplayFields(item);
  const article = document.createElement("article");
  article.className = `message message-${item.direction}`;
  article.dataset.key = item.key;
  article.tabIndex = 0;
  if (item.delivery === "failed") article.classList.add("message-failed");
  if (item.key === selectedKey) article.classList.add("message-selected");

  const unread = item.direction === "inbound" && !item.historyId && !readKeys.has(item.key);
  article.setAttribute("aria-label", `${unread ? "未読 " : ""}${fields.timestamp} ${fields.sender} ${fields.message}`);
  const { icon, body: listBody } = splitMessageIcon(fields.message);
  addText(article, "message-icon", icon).setAttribute("aria-hidden", "true");
  const time = document.createElement("time");
  time.className = "message-time";
  time.classList.toggle("message-time-unread", unread);
  setText(time, fields.timestamp);
  article.append(time);

  const head = document.createElement("header");
  head.className = "message-head";
  addText(head, "message-callsign", fields.sender);
  addText(head, "delivery", deliveryLabel(item));

  const body = document.createElement("p");
  body.className = "message-body";
  addText(body, "message-full-text", fields.message);
  addText(body, "message-list-text", listBody);

  const meta = document.createElement("footer");
  meta.className = "message-meta";
  const symbolRate = item.symbolRate ?? (item.robust ? 20 : 40);
  addText(meta, symbolRate < 40 ? "robust-tag" : "", `${symbolRate} sym/s`);
  addText(meta, "", `#${item.sequence}`);
  if (Number.isFinite(item.syncScore)) addText(meta, "", `sync ${item.syncScore.toFixed(2)}`);
  if (item.errorMessage) addText(meta, "", item.errorMessage);

  addText(article, "message-source", fields.source);
  const quality = qualitySummary(item);
  addText(article, "message-quality", `${quality.ber}\n${quality.snr}`);
  addText(article, "message-crc", quality.crc);
  addText(article, "message-session", fields.session);
  addText(article, "message-sequence", fields.sequence);

  article.append(head, body, meta);
  const select = () => {
    if (selectedKey === item.key) return;
    selectedKey = item.key;
    readKeys.add(item.key);
    elements.timeline.querySelector(".message-selected")?.classList.remove("message-selected");
    article.classList.add("message-selected");
    time.classList.remove("message-time-unread");
    article.setAttribute("aria-label", `${fields.timestamp} ${fields.sender} ${fields.message}`);
    elements["message-detail"].scrollTop = 0;
    renderMessageDetail(item);
  };
  article.addEventListener("click", select);
  article.addEventListener("dblclick", () => {
    select();
    setMessageDetailOpen(true);
  });
  article.addEventListener("keydown", (event) => {
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      select();
      if (event.key === "Enter") setMessageDetailOpen(true);
    }
  });
  return article;
}

function setMessageDetailOpen(open) {
  elements["message-detail"].hidden = !open;
  const toggle = elements["message-detail-toggle"];
  toggle.setAttribute("aria-expanded", String(open));
  setText(toggle, open ? "閉じる" : "開く");
}

elements["message-detail-toggle"].addEventListener("click", () => {
  setMessageDetailOpen(elements["message-detail"].hidden);
});

const resizePanels = [
  elements.timeline.closest(".timeline-panel"),
  elements["message-detail"],
  elements["receive-preview"],
  elements["message-input"],
];

function measureResizePanel(panel) {
  const style = getComputedStyle(panel);
  return {
    panel,
    height: panel.getBoundingClientRect().height,
    min: parseFloat(style.minHeight) || 0,
    max: style.maxHeight === "none" ? Infinity : parseFloat(style.maxHeight),
  };
}

for (const handle of document.querySelectorAll("[data-resize-boundary]")) {
  const boundary = Number(handle.dataset.resizeBoundary);
  const above = () => resizePanels.slice(0, boundary).findLast((panel) => !panel.hidden);
  const below = () => resizePanels.slice(boundary).find((panel) => !panel.hidden);
  let drag = null;
  handle.addEventListener("pointerdown", (event) => {
    if (event.button !== 0 || !event.isPrimary || drag) return;
    event.preventDefault();
    drag = {
      pointerId: event.pointerId,
      y: event.clientY,
      upper: measureResizePanel(above()),
      lower: measureResizePanel(below()),
    };
    handle.setPointerCapture(event.pointerId);
  });
  handle.addEventListener("pointermove", (event) => {
    if (drag?.pointerId !== event.pointerId) return;
    const { upper, lower, y } = drag;
    if (above() !== upper.panel || below() !== lower.panel) {
      handle.releasePointerCapture(event.pointerId);
      return;
    }
    const minDelta = Math.max(upper.min - upper.height, lower.height - lower.max);
    const maxDelta = Math.min(upper.max - upper.height, lower.height - lower.min);
    const delta = Math.max(minDelta, Math.min(maxDelta, event.clientY - y));
    if (upper.panel !== resizePanels[0]) upper.panel.style.height = `${upper.height + delta}px`;
    lower.panel.style.height = `${lower.height - delta}px`;
  });
  handle.addEventListener("lostpointercapture", () => {
    drag = null;
  });
}

function renderMessageDetail(item) {
  const detail = elements["message-detail"];
  const scrollTop = detail.scrollTop;
  detail.replaceChildren();
  if (!item) {
    addText(detail, "message-detail-empty", "履歴からメッセージを選択してください");
    return;
  }
  const fields = messageDisplayFields(item);
  const header = document.createElement("header");
  header.className = "message-detail-header";
  addText(header, "message-detail-time", fields.timestamp);
  addText(header, "message-callsign", fields.sender);
  addText(header, "delivery", deliveryLabel(item));
  const content = document.createElement("div");
  content.className = "message-detail-content";
  content.append(header);
  addText(content, "message-body message-detail-body", fields.message);
  detail.append(content);
  addText(detail, "message-detail-metrics", [
    `Source   ${fields.source}`,
    `Session  ${fields.session}`,
    `Sequence ${fields.sequence}`,
    fields.quality,
  ].join("\n"));
  detail.scrollTop = scrollTop;
}

function renderDetails(status) {
  elements["detail-list"].replaceChildren();
  if (!status) return;
  const entries = [
    ["Session", status.session_id],
    ["Input", `${status.input_device ?? "—"}${status.input_device_name ? ` ${status.input_device_name}` : ""} / ${status.input_rate == null ? "—" : `${status.input_rate} Hz`}`],
    ["Output", `${status.output_device ?? "—"}${status.output_device_name ? ` ${status.output_device_name}` : ""} / ${status.output_rate == null ? "—" : `${status.output_rate} Hz`}`],
    ["Delay", `${status.tx_delay_ms ?? 250} ms`],
    ["Tail", `${status.tx_tail_ms ?? 250} ms`],
    ["Queue", `${status.queue_depth ?? "—"} / ${status.queue_capacity ?? "—"}`],
  ];
  for (const [name, value] of entries) {
    const term = document.createElement("dt");
    const description = document.createElement("dd");
    setText(term, name);
    setText(description, value);
    elements["detail-list"].append(term, description);
  }
}

function renderNotice() {
  if (!state.notice) {
    elements.notice.hidden = true;
    return;
  }
  const text =
    state.notice.kind === "gap"
      ? `受信イベントを${state.notice.dropped}件読み飛ばしました。`
      : `${state.notice.component ?? "service"}: ${state.notice.message ?? state.notice.code ?? "diagnostic"}`;
  setText(elements.notice, text);
  elements.notice.hidden = false;
}

function renderTimeline() {
  const shouldFollow = nearTimelineBottom();
  const fragment = document.createDocumentFragment();
  for (const item of state.items) fragment.append(messageElement(item));
  elements.timeline.replaceChildren(fragment);
  if (state.items.length === 0) elements.timeline.append(elements["empty-state"]);
  elements["empty-state"].hidden = state.items.length !== 0;
  const selected = state.items.find((item) => item.key === selectedKey);
  renderMessageDetail(selected);
  if (shouldFollow) {
    elements.timeline.scrollTop = elements.timeline.scrollHeight;
    elements["new-messages"].hidden = true;
  } else {
    elements["new-messages"].hidden = false;
  }
}

function updateComposer() {
  const bytes = utf8Length(elements["message-input"].value);
  const invalid = bytes > 280;
  const callsign = state.status?.callsign;
  const estimate = callsign
    ? formatTransmitEstimate(
      bytes,
      utf8Length(callsign),
      selectedSymbolRate(),
      state.status?.tx_delay_ms ?? 250,
      state.status?.tx_wake_ms ?? 600,
      state.status?.tx_tail_ms ?? 250,
    )
    : null;
  setText(elements["byte-count"], `${bytes} / 280 bytes · 送信 ${estimate ?? "約--秒"}`);
  elements["byte-count"].classList.toggle("byte-count-invalid", invalid);
  elements["message-input"].setAttribute("aria-invalid", String(invalid));
  const txAvailable = state.status?.tx?.state === "running";
  elements["send-button"].disabled = invalid || submitPending || !txAvailable;
}

function render() {
  const status = state.status;
  setText(elements.callsign, status?.callsign ?? "--------");
  chip(elements["connection-status"], status?.history_polling ? "HTTP" : "SSE", connection);
  chip(elements["service-state"], "Service", status?.state);
  chip(elements["rx-state"], "RX", status?.rx?.state);
  chip(elements["tx-state"], "TX", status?.tx?.state);
  elements["settings-button"].disabled = Boolean(status?.history_polling && !status.settings_commands);
  const ptt = status?.ptt;
  const pttChip = elements["ptt-state"];
  pttChip.hidden = !ptt;
  pttChip.classList.toggle("ptt-closed", Boolean(ptt?.port && ptt.is_open === false));
  pttChip.title = ptt?.error ?? (ptt?.port ? ptt.line.toUpperCase() : "");
  setText(pttChip, ptt?.port
    ? `PTT:${ptt.port} ${ptt.is_open === null ? "BUSY" : ptt.is_open ? "OPEN" : "CLOSED"}`
    : "PTT OFF");
  renderDetails(status);
  renderNotice();
  renderTimeline();
  updateComposer();
}

async function refreshStatus() {
  try {
    const response = await fetch("/api/v1/status", { cache: "no-store" });
    if (!response.ok) throw new Error(`status ${response.status}`);
    state = reduceStatus(state, await response.json());
    if (elements["composer-status"].dataset.source === "status") {
      setText(elements["composer-status"], "");
      elements["composer-status"].classList.remove("composer-status-error");
      delete elements["composer-status"].dataset.source;
    }
    render();
  } catch (error) {
    setText(elements["composer-status"], `状態取得失敗: ${error.message}`);
    elements["composer-status"].classList.add("composer-status-error");
    elements["composer-status"].dataset.source = "status";
    render();
  }
}

async function refreshHistory(polling = false) {
  try {
    const recent = polling && (historyCursor === 0 || refreshRecentHistory);
    const afterId = polling && !recent ? historyCursor : 0;
    const latest = recent ? "&latest=1" : "";
    const response = await fetch(`/api/v1/messages?after_id=${afterId}&limit=256${latest}`, {
      cache: "no-store",
    });
    if (!response.ok) throw new Error(`history ${response.status}`);
    const payload = await response.json();
    let messages = payload.messages ?? [];
    const connectionChanged = polling && connection !== "live";
    const restarted = polling && historyGeneration !== null &&
      payload.history_generation !== historyGeneration;
    if (polling) {
      historyGeneration = payload.history_generation ?? null;
      refreshRecentHistory = !recent && !restarted && historyRevision !== null &&
        payload.history_revision - historyRevision > messages.length;
      historyRevision = payload.history_revision ?? null;
      historyCursor = restarted ? 0 : (payload.last_id ?? afterId);
      if (restarted) {
        state = { ...state, items: [] };
        selectedKey = null;
        readKeys.clear();
        monitorRevision = 0;
        messages = [];
      }
      connection = "live";
      if (elements["composer-status"].dataset.source === "history") {
        setText(elements["composer-status"], "");
        elements["composer-status"].classList.remove("composer-status-error");
        delete elements["composer-status"].dataset.source;
      }
    }
    if (!polling || messages.length || connectionChanged || restarted) {
      state = reduceHistory(state, messages);
      render();
    }
  } catch (error) {
    setText(elements["composer-status"], `履歴取得失敗: ${error.message}`);
    elements["composer-status"].classList.add("composer-status-error");
    if (polling) {
      elements["composer-status"].dataset.source = "history";
      connection = "retrying";
      chip(elements["connection-status"], "HTTP", connection);
    }
  }
}

async function pollHistory() {
  await refreshHistory(true);
  setTimeout(pollHistory, 1000);
}

function parseEvent(event, type) {
  try {
    state = reduceEvent(state, {
      id: event.lastEventId ? Number(event.lastEventId) : null,
      type,
      data: JSON.parse(event.data),
    });
    if (["service_started", "diagnostic", "gap"].includes(type)
        || (type === "transmitted" && state.status?.ptt?.port && !state.status.ptt.is_open)) {
      refreshStatus();
    }
    render();
  } catch (error) {
    setText(elements["composer-status"], `イベント解釈失敗: ${error.message}`);
    elements["composer-status"].classList.add("composer-status-error");
  }
}

function connectEvents() {
  const source = new EventSource("/api/v1/events?after_id=0");
  source.onopen = () => {
    connection = "live";
    render();
  };
  source.onerror = () => {
    connection = "retrying";
    render();
  };
  for (const type of [
    "service_started",
    "receive",
    "transmit_queued",
    "transmitted",
    "transmit_failed",
    "diagnostic",
    "gap",
  ]) {
    source.addEventListener(type, (event) => parseEvent(event, type));
  }
}

async function sendMessage() {
  if (submitPending || state.status?.tx?.state !== "running" ||
      utf8Length(elements["message-input"].value) > 280) return;
  submitPending = true;
  delete elements["composer-status"].dataset.source;
  elements["composer-status"].classList.remove("composer-status-error");
  setText(elements["composer-status"], "送信キューへ登録中…");
  updateComposer();
  const request = {
    message: elements["message-input"].value,
    robust: selectedSymbolRate() < 40,
    symbol_rate: selectedSymbolRate(),
  };
  try {
    const response = await fetch("/api/v1/messages", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(request),
    });
    const payload = await response.json();
    if (!response.ok) throw new Error(payload.detail?.message ?? `HTTP ${response.status}`);
    state = reduceAccepted(state, request, payload);
    elements["message-input"].value = "";
    setText(elements["composer-status"], "送信キューへ登録しました。");
    render();
  } catch (error) {
    setText(elements["composer-status"], `送信失敗: ${error.message}`);
    elements["composer-status"].classList.add("composer-status-error");
  } finally {
    submitPending = false;
    updateComposer();
  }
}

elements["composer-form"].addEventListener("submit", (event) => {
  event.preventDefault();
  sendMessage();
});
elements["message-input"].addEventListener("input", updateComposer);
for (const rate of [40, 20, 10, 4]) {
  elements[`mode-${rate}`].addEventListener("change", updateComposer);
}
elements["message-input"].addEventListener("keydown", (event) => {
  if (event.ctrlKey && event.key === "Enter") {
    event.preventDefault();
    elements["composer-form"].requestSubmit();
  }
});
elements["new-messages"].addEventListener("click", () => {
  elements.timeline.scrollTop = elements.timeline.scrollHeight;
  elements["new-messages"].hidden = true;
});
elements["monitor-toggle"].addEventListener("click", () => setMonitorOpen(true));
elements["monitor-close"].addEventListener("click", () => setMonitorOpen(false));
elements["monitor-backdrop"].addEventListener("click", () => setMonitorOpen(false));
const settingsDialog = createSettingsDialog({ onChanged: refreshStatus });
elements["settings-button"].addEventListener("click", () => {
  settingsDialog.open(elements["settings-button"]);
});
elements["timeline-view-list"].addEventListener("click", () => setTimelineView("list"));
elements["timeline-view-messenger"].addEventListener("click", () =>
  setTimelineView("messenger"),
);

async function bootstrap() {
  setTimelineView(timelineView, false);
  render();
  await refreshStatus();
  if (state.status?.history_polling) {
    pollHistory();
    await refreshMonitor();
    setInterval(refreshMonitor, 250);
    setInterval(refreshStatus, 30_000);
    return;
  }
  await refreshHistory();
  await refreshMonitor();
  connectEvents();
  setInterval(refreshStatus, 30_000);
  setInterval(refreshMonitor, 250);
}

bootstrap();
