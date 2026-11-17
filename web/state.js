const MAX_ITEMS = 256;
const MAX_EVENT_IDS = 1024;

export function utf8Length(text) {
  return new TextEncoder().encode(text).length;
}

export function estimateTransmitSeconds(messageBytes, callsignBytes, symbolRate, delayMs = 250, wakeMs = 600, tailMs = 250) {
  return estimateTransmitBreakdown(messageBytes, callsignBytes, symbolRate, delayMs, wakeMs, tailMs).total;
}

function tenth(value) {
  return Math.round(value * 10) / 10;
}

export function estimateTransmitBreakdown(messageBytes, callsignBytes, symbolRate, delayMs = 250, wakeMs = 600, tailMs = 250) {
  const rate = typeof symbolRate === "number" ? symbolRate : (symbolRate ? 20 : 40);
  const bodyBytes = callsignBytes + messageBytes + 4;
  const bodyCodedBits = 2 * (bodyBytes * 8 + 6);
  const fullBlocks = Math.floor(bodyCodedBits / 256);
  const tailBits = bodyCodedBits % 256;
  const transmittedBits = fullBlocks * 256 + (tailBits ? Math.ceil(tailBits / 16) * 16 : 0);
  const bodySymbols = transmittedBits / 4;
  const midSymbols = 4 * Math.ceil(bodySymbols / 256);
  const result = {
    before: delayMs / 1000,
    wake: tenth(wakeMs / 1000),
    sync: tenth(12 / rate),
    header: tenth(100 / rate),
    callsign: tenth((4 * callsignBytes) / rate),
    payload: tenth((4 * messageBytes) / rate),
    fecCrc: tenth(20 / rate),
    mid: tenth(midSymbols / rate),
    after: tailMs / 1000,
  };
  return {
    ...result,
    total: tenth(Object.values(result).reduce((sum, seconds) => sum + seconds, 0)),
  };
}

export function formatTransmitEstimate(messageBytes, callsignBytes, symbolRate, delayMs = 250, wakeMs = 600, tailMs = 250) {
  const value = estimateTransmitBreakdown(messageBytes, callsignBytes, symbolRate, delayMs, wakeMs, tailMs);
  return `約${value.total.toFixed(1)}秒（Delay ${value.before.toFixed(1)} + Wake ${value.wake.toFixed(1)} + Sync ${value.sync.toFixed(1)} + Header ${value.header.toFixed(1)} + Callsign ${value.callsign.toFixed(1)} + Payload ${value.payload.toFixed(1)} + CRC/FEC ${value.fecCrc.toFixed(1)} + MID ${value.mid.toFixed(1)} + Tail ${value.after.toFixed(1)}）`;
}

export function normalizeTimelineView(value) {
  return value === "messenger" ? "messenger" : "list";
}

function formatObservedAt(seconds) {
  if (!Number.isFinite(seconds) || seconds <= 0) return "—";
  const parts = new Intl.DateTimeFormat("sv-SE", {
    timeZone: "Asia/Tokyo",
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hourCycle: "h23",
  }).format(new Date(seconds * 1000));
  return parts.replace("-", "/").replace("-", "/");
}

const QUALITY_RESERVED_KEYS = new Set([
  "metrics_schema", "decode_path", "rewind_seconds", "sync_matches",
  "sync_symbols", "sync_score", "mid_score", "frequency_offset_hz",
  "symbol_confidence_mean", "symbol_confidence_min",
  "pre_fec_bit_errors_est", "pre_fec_bits", "pre_fec_ber_est",
  "snr_estimate_db", "snr_2500hz_estimate_db", "refinement_improvements", "refinement_invocations",
  "crc", "packet_timestamp", "robust", "symbol_rate", "verification",
  "observation_id",
  "raw_packet_hex",
]);

function finite(value) {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

function metricLine(label, value) {
  return `${label.padEnd(7)}${value}`;
}

export function qualityDisplayLines(item) {
  const metadata = item?.metadata && typeof item.metadata === "object"
    ? item.metadata
    : {};
  const lines = [];
  const rewind = finite(metadata.rewind_seconds);
  if (metadata.decode_path) {
    const path = metadata.decode_path === "mid_recovery" ? "MID" : "HEAD";
    lines.push(metricLine("PATH", path));
    if (rewind !== null) lines.push(metricLine("REWIND", `${rewind.toFixed(1)} s`));
  }
  const matches = finite(metadata.sync_matches);
  const symbols = finite(metadata.sync_symbols);
  const score = finite(
    metadata.sync_score
      ?? (metadata.decode_path === "mid_recovery" ? null : item?.syncScore),
  );
  if (matches !== null && symbols !== null) {
    lines.push(metricLine("SYNC", `${matches}/${symbols}  ${score === null ? "—" : score.toFixed(2)}`));
  } else if (score !== null) {
    lines.push(metricLine("SYNC", `—    ${score.toFixed(2)}`));
  }
  const midScore = finite(metadata.mid_score);
  if (midScore !== null) lines.push(metricLine("MIDQ", midScore.toFixed(2)));
  const frequency = finite(metadata.frequency_offset_hz);
  if (frequency !== null) {
    const signedFrequency = `${frequency >= 0 ? "+" : ""}${frequency.toFixed(1)}`;
    lines.push(metricLine("FREQ", `${signedFrequency} Hz`));
  }
  const confidenceMean = finite(metadata.symbol_confidence_mean);
  const confidenceMin = finite(metadata.symbol_confidence_min);
  if (confidenceMean !== null || confidenceMin !== null) {
    lines.push(metricLine("CONF", `${confidenceMean?.toFixed(2) ?? "—"}/${confidenceMin?.toFixed(2) ?? "—"}`));
  }
  const ber = finite(metadata.pre_fec_ber_est);
  if (ber !== null) lines.push(metricLine("BER", `${(ber * 100).toFixed(2)} % pre-FEC`));
  const bitErrors = finite(metadata.pre_fec_bit_errors_est);
  const bitCount = finite(metadata.pre_fec_bits);
  if (bitErrors !== null) {
    lines.push(metricLine("FEC", `${bitErrors}${bitCount === null ? "" : `/${bitCount}`} bit diff`));
  }
  const snr = finite(metadata.snr_estimate_db);
  const snr2500 = finite(metadata.snr_2500hz_estimate_db);
  if (snr !== null) {
    const reference = snr2500 === null ? "" : ` (${snr2500.toFixed(1)} dB/2.5k)`;
    lines.push(metricLine("SNR", `${snr.toFixed(1)} dB${reference} est`));
  }
  const improvements = finite(metadata.refinement_improvements);
  const invocations = finite(metadata.refinement_invocations);
  if (improvements !== null || invocations !== null) {
    lines.push(metricLine("REFINE", `${improvements ?? "—"}/${invocations ?? "—"}`));
  }
  if (metadata.crc !== undefined) lines.push(metricLine("CRC", String(metadata.crc).toUpperCase()));
  for (const key of Object.keys(metadata).sort()) {
    if (QUALITY_RESERVED_KEYS.has(key)) continue;
    const value = metadata[key];
    lines.push(`${key} ${typeof value === "object" ? JSON.stringify(value) : String(value)}`);
  }
  return lines;
}

export function messageDisplayFields(item) {
  const source = item.protocol ?? (item.robust ? "mu16tty-20" : "mu16tty-40");
  const quality = qualityDisplayLines(item).join("\n") || "—";
  return {
    timestamp: formatObservedAt(item.observedAt ?? item.packetTimestamp),
    sender: item.callsign || "自局",
    message: item.message ?? "",
    source,
    quality,
    session: item.sessionId ?? "—",
    sequence: item.sequence ?? "—",
  };
}

const graphemeSegmenter = typeof Intl.Segmenter === "function"
  ? new Intl.Segmenter(undefined, { granularity: "grapheme" })
  : null;

export function splitMessageIcon(message) {
  const icon = graphemeSegmenter?.segment(message).containing(0)?.segment ?? "";
  const emoji = /^(?:\p{Emoji_Presentation}|\p{Emoji}\uFE0F|[0-9#*]\uFE0F?\u20E3)/u;
  if (!emoji.test(icon) || icon.includes("\uFE0E")) return { icon: "", body: message };
  return { icon, body: message.slice(icon.length) };
}

export function qualitySummary(item) {
  const metadata = item.metadata ?? {};
  const ber = finite(metadata.pre_fec_ber_est);
  const snr2500 = finite(metadata.snr_2500hz_estimate_db);
  const snr = finite(metadata.snr_estimate_db);
  return {
    ber: `BER ${ber === null ? "—" : `${(ber * 100).toFixed(2)}%`}`,
    snr: `SNR ${snr2500 !== null ? `${snr2500.toFixed(1)} dB/2.5k` : snr !== null ? `${snr.toFixed(1)} dB` : "—"}`,
    crc: metadata.crc == null ? "—" : String(metadata.crc).toUpperCase(),
  };
}

export function createState() {
  return {
    status: null,
    currentSession: null,
    items: [],
    seenEventIds: [],
    notice: null,
  };
}

export function reduceStatus(state, status) {
  return {
    ...state,
    status: { ...status },
    currentSession: status.session_id ?? state.currentSession,
  };
}

function identityKey(sessionId, sequence) {
  return `${sessionId}:${sequence}`;
}

function upsertItem(state, key, values) {
  const index = state.items.findIndex((item) => item.key === key);
  let items;
  if (index < 0) {
    items = [...state.items, { key, ...values }];
  } else {
    items = state.items.map((item, itemIndex) =>
      itemIndex === index ? { ...item, ...values, key } : item,
    );
  }
  return { ...state, items: items.slice(-MAX_ITEMS) };
}

function findItem(state, key) {
  return state.items.find((item) => item.key === key);
}

function receivedIdentityKey(sessionId, sequence, metadata, observationId) {
  const key = identityKey(sessionId, sequence);
  if (metadata?.verification !== "damaged") return key;
  const discriminator = metadata.observation_id ?? observationId ?? metadata.raw_packet_hex ?? "candidate";
  return `${key}:damaged:${discriminator}`;
}

export function reduceAccepted(state, request, accepted) {
  const key = identityKey(accepted.session_id, accepted.sequence);
  const existing = findItem(state, key);
  return upsertItem(state, key, {
    direction: "outbound",
    callsign: state.status?.callsign ?? "",
    message: request.message,
    robust: request.robust,
    symbolRate: request.symbol_rate ?? (request.robust ? 20 : 40),
    sessionId: accepted.session_id,
    sequence: accepted.sequence,
    delivery: existing?.delivery ?? "queued",
    queueDepth: accepted.queue_depth_after_accept,
  });
}

export function reduceHistory(state, records) {
  let next = state;
  for (const record of records) {
    const key = receivedIdentityKey(record.session_id, record.sequence, record.metadata, record.id);
    const outbound = record.direction === "outbound";
    const existing = findItem(next, key);
    const ownHistory = record.metadata?.delivery && record.session_id === next.currentSession;
    const loopback = ownHistory && existing &&
      (existing.delivery === "loopback_received" || existing.direction !== record.direction);
    next = upsertItem(next, key, {
      direction: outbound || loopback ? "outbound" : "inbound",
      callsign: record.sender ?? "",
      message: record.message ?? "",
      robust: record.metadata?.robust ?? record.protocol === "mu16tty-20",
      sessionId: record.session_id,
      sequence: record.sequence,
      delivery: record.metadata?.delivery === "failed" ? "failed"
        : loopback || record.metadata?.delivery === "loopback_received"
          ? "loopback_received" : outbound ? "sent" : "received",
      packetTimestamp: record.metadata?.packet_timestamp,
      symbolRate: record.metadata?.symbol_rate,
      syncScore: record.quality,
      protocol: record.protocol,
      historyId: record.id,
      observedAt: record.observed_at,
      metadata: record.metadata ?? {},
    });
  }
  return next;
}

function withSeenEvent(state, eventId) {
  if (!Number.isInteger(eventId)) {
    return state;
  }
  return {
    ...state,
    seenEventIds: [...state.seenEventIds, eventId].slice(-MAX_EVENT_IDS),
  };
}

function outboundValues(state, data, delivery) {
  const key = identityKey(data.session_id, data.sequence);
  const existing = findItem(state, key);
  const finalDelivery =
    delivery === "queued" && existing?.delivery && existing.delivery !== "queued"
      ? existing.delivery
      : delivery;
  return [
    key,
    {
      direction: "outbound",
      callsign: state.status?.callsign ?? "",
      message: data.message ?? existing?.message ?? "",
      robust: data.robust ?? existing?.robust ?? false,
      sessionId: data.session_id,
      sequence: data.sequence,
      delivery: finalDelivery,
      queueDepth: data.queue_depth_after_accept ?? existing?.queueDepth,
      errorCode: data.code ?? existing?.errorCode,
      errorMessage: data.error ?? existing?.errorMessage,
      observedAt: data.observed_at ?? existing?.observedAt,
    },
  ];
}

export function reduceEvent(state, event) {
  if (Number.isInteger(event.id) && state.seenEventIds.includes(event.id)) {
    return state;
  }
  let next = withSeenEvent(state, event.id);
  const data = event.data ?? {};

  if (event.type === "service_started") {
    return { ...next, currentSession: data.session_id ?? next.currentSession };
  }
  if (event.type === "gap") {
    return { ...next, notice: { kind: "gap", dropped: data.dropped ?? 0 } };
  }
  if (event.type === "diagnostic") {
    return {
      ...next,
      notice: {
        kind: "diagnostic",
        component: data.component,
        code: data.code,
        message: data.message,
      },
    };
  }
  if (event.type === "transmit_queued") {
    const [key, values] = outboundValues(next, data, "queued");
    return upsertItem(next, key, values);
  }
  if (event.type === "transmitted") {
    const [key, values] = outboundValues(next, data, "sent");
    return upsertItem(next, key, values);
  }
  if (event.type === "transmit_failed") {
    const [key, values] = outboundValues(next, data, "failed");
    return upsertItem(next, key, values);
  }
  if (event.type === "receive") {
    const key = receivedIdentityKey(data.session_id, data.sequence, data.metadata, event.id);
    const existing = findItem(next, key);
    if (data.session_id === next.currentSession && existing?.direction === "outbound") {
      return upsertItem(next, key, {
        delivery: "loopback_received",
        symbolRate: data.symbol_rate,
        syncScore: data.sync_score,
        packetTimestamp: data.packet_timestamp,
        observedAt: data.observed_at,
        metadata: data.metadata ?? existing.metadata ?? {},
      });
    }
    return upsertItem(next, key, {
      direction: "inbound",
      callsign: data.callsign ?? "",
      message: data.message ?? "",
      robust: data.robust ?? false,
      sessionId: data.session_id,
      sequence: data.sequence,
      packetTimestamp: data.packet_timestamp,
      observedAt: data.observed_at,
      symbolRate: data.symbol_rate,
      syncScore: data.sync_score,
      protocol: data.protocol,
      delivery: "received",
      metadata: data.metadata ?? {},
    });
  }
  return next;
}
