export function acceptMonitorSnapshot(previousRevision, payload) {
  const revision = Number(payload?.revision);
  return {
    accepted: payload?.available === true && Number.isInteger(revision) && revision > previousRevision,
    revision: payload?.available === true && revision > previousRevision ? revision : previousRevision,
  };
}

export function dbfsToColor(value) {
  const db = Number.isFinite(value) ? Math.max(-70, Math.min(-5, value)) : -70;
  const level = (db + 70) / 65;
  if (level < 0.25) {
    return [0, 0, Math.round(15 + 180 * (level / 0.25))];
  }
  if (level < 0.5) {
    const step = (level - 0.25) / 0.25;
    return [0, Math.round(255 * step), Math.round(195 + 60 * step)];
  }
  if (level < 0.75) {
    const step = (level - 0.5) / 0.25;
    return [Math.round(255 * step), 255, Math.round(255 * (1 - step))];
  }
  const step = (level - 0.75) / 0.25;
  return [255, Math.round(255 * (1 - step)), 0];
}

const DECODER_LABELS = {
  idle: "IDLE",
  search: "SEARCH",
  acquire: "ACQUIRE",
  header: "HEADER",
  receive: "RECEIVE",
  verify: "VERIFY",
  queued: "CAPTURE",
  sync_search: "SYNC SEARCH",
  frame_check: "FRAME CHECK",
  found: "FOUND",
  hold: "HOLD",
  combine: "COMBINE",
  recover: "RECOVER",
  retry: "RETRY",
  success: "DECODED",
  truncated: "TIMEOUT",
  skipped: "SKIPPED",
};

const REJECTION_LABELS = {
  no_sync: "NO SYNC",
  fec_crc: "FEC/CRC FAIL",
  mode_mismatch: "MODE MISMATCH",
  header_expired: "HEADER EXPIRED",
  header_decode_failed: "HEADER DECODE FAIL",
  header_invalid: "INVALID HEADER",
  header_rejected: "HEADER REJECTED",
};

export function decoderDisplayRows(decoders, nowMs = Date.now()) {
  const active = new Set([
    "search", "acquire", "header", "receive", "verify",
    "queued", "sync_search", "frame_check", "found", "hold", "combine", "recover", "retry",
  ]);
  return (Array.isArray(decoders) ? decoders : []).map((decoder) => {
    const phase = typeof decoder.phase === "string" ? decoder.phase : "idle";
    const startedMs = Number(decoder.started_at) * 1000;
    const elapsed = active.has(phase) && Number.isFinite(startedMs)
      ? `${Math.max(0, (nowMs - startedMs) / 1000).toFixed(1)}s`
      : "";
    const baseLabel = phase === "rejected"
      ? (REJECTION_LABELS[decoder.detail] ?? "REJECTED")
      : (DECODER_LABELS[phase] ?? "IDLE");
    const label = decoder.lane === "M" && decoder.detail && phase !== "rejected"
      ? `${baseLabel} ${decoder.detail}`
      : baseLabel;
    return {
      key: `${decoder.symbol_rate ?? decoder.name ?? "decoder"}${decoder.lane ?? ""}`,
      name: String(decoder.name ?? `Decoder ${decoder.symbol_rate ?? ""}`).trim(),
      phase,
      label,
      elapsed,
    };
  });
}

function fitCanvas(canvas, fallbackWidth, fallbackHeight) {
  const width = Math.max(1, Math.round(canvas.clientWidth || fallbackWidth));
  const height = Math.max(1, Math.round(canvas.clientHeight || fallbackHeight));
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
    return true;
  }
  return false;
}

export function drawWaveform(canvas, samples, clipped = false) {
  fitCanvas(canvas, 260, 72);
  const context = canvas.getContext("2d");
  context.clearRect(0, 0, canvas.width, canvas.height);
  context.strokeStyle = clipped ? "hsl(0 100% 50%)" : "#45d6d1";
  context.lineWidth = 1;
  context.beginPath();
  const values = Array.isArray(samples) ? samples : [];
  values.forEach((value, index) => {
    const x = values.length <= 1 ? 0 : (index * (canvas.width - 1)) / (values.length - 1);
    const y = canvas.height / 2 - Number(value || 0) * canvas.height * 0.46;
    if (index === 0) context.moveTo(x, y);
    else context.lineTo(x, y);
  });
  context.stroke();
}

export function appendWaterfall(canvas, spectrum) {
  const resized = fitCanvas(canvas, 260, 150);
  const context = canvas.getContext("2d");
  if (!resized) context.drawImage(canvas, 0, -1);
  const values = Array.isArray(spectrum) ? spectrum : [];
  const row = context.createImageData(canvas.width, 1);
  for (let x = 0; x < canvas.width; x += 1) {
    const index = Math.min(values.length - 1, Math.floor((x * values.length) / canvas.width));
    const [red, green, blue] = dbfsToColor(values[index]);
    const offset = x * 4;
    row.data[offset] = red;
    row.data[offset + 1] = green;
    row.data[offset + 2] = blue;
    row.data[offset + 3] = 255;
  }
  context.putImageData(row, 0, canvas.height - 1);
}
