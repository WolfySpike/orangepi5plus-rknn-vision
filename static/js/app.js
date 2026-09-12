const defaultLabels = ["Body", "Head", "Thermal", "Teammate", "Bot", "Knocked", "D_Body", "D_Head", "Neg"];

let appState = { params: {}, runtime: {}, models: [], profiles: [] };
let selectedFamily = "yolov8";
let familyDirty = false;
let pendingFamily = null;
let selectedNpuMode = "latency";
let npuModeDirty = false;
let pendingNpuMode = null;
let modelSelectDirty = false;
let pendingModelId = null;
let configSelectDirty = false;
let pendingConfigName = null;
let lastPreviewRefreshAt = 0;
let streamerConfigDirty = false;
let streamerSaveTimer = null;
let streamerLastEditAt = 0;

function switchTab(tab) {
  document.querySelectorAll(".section").forEach((e) => e.classList.remove("active"));
  document.querySelectorAll(".tab-btn").forEach((e) => e.classList.remove("active"));
  const target = document.getElementById("tab-" + tab);
  const btn = document.querySelector(`.tab-btn[data-tab="${tab}"]`);
  if (target) target.classList.add("active");
  if (btn) btn.classList.add("active");
  if (tab === "system") fetchLogs(true);
}

function fmt(k, v) {
  const n = Number(v);
  if (
    k.includes("_py") ||
    k.includes("_pri") ||
    k === "pred_lead_ms" ||
    k === "pred_max_speed" ||
    k === "aim_fov" ||
    k === "trigger_radius" ||
    k === "trigger_delay_ms" ||
    k === "preview_fps" ||
    k === "aim_slow_radius" ||
    k.startsWith("xhair_")
  ) {
    return n.toFixed(0);
  }
  if (k === "deadzone") {
    return n.toFixed(1);
  }
  return n.toFixed(2);
}

function escapeHtml(s) {
  return String(s)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

function getClassName(i) {
  const arr = appState.runtime.class_names || [];
  if (i < arr.length && String(arr[i]).trim()) return String(arr[i]).trim();
  if (i < defaultLabels.length) return defaultLabels[i];
  return `Class_${i}`;
}

function getAimMask() {
  const raw = Number(appState.params.aim_mask ?? 3);
  return Math.max(1, Math.min(31, Math.round(raw)));
}

function renderAimMask(mask) {
  const m = Math.max(1, Math.min(31, Number(mask) || 3));
  const set = (id, bit) => {
    const el = document.getElementById(id);
    if (el) el.checked = (m & bit) !== 0;
  };
  set("aim_key_l", 1);
  set("aim_key_r", 2);
  set("aim_key_m", 4);
  set("aim_key_su", 8);
  set("aim_key_sd", 16);
}

function toggleAimKey(bit, checked) {
  let m = getAimMask();
  if (checked) m |= bit;
  else m &= ~bit;
  if (m === 0) m = 1;
  sendVal("aim_mask", m);
  renderAimMask(m);
}

function getTriggerMask() {
  const raw = Number(appState.params.trigger_mask ?? 4);
  return Math.max(0, Math.min(31, Math.round(raw)));
}

function renderTriggerMask(mask) {
  const m = Math.max(0, Math.min(31, Number(mask) || 0));
  const set = (id, bit) => {
    const el = document.getElementById(id);
    if (el) el.checked = (m & bit) !== 0;
  };
  set("trigger_key_l", 1);
  set("trigger_key_r", 2);
  set("trigger_key_m", 4);
  set("trigger_key_su", 8);
  set("trigger_key_sd", 16);
}

function toggleTriggerKey(bit, checked) {
  let m = getTriggerMask();
  if (checked) m |= bit;
  else m &= ~bit;
  sendVal("trigger_mask", m);
  renderTriggerMask(m);
}

function isFocusedId(id) {
  const ae = document.activeElement;
  return !!ae && ae.id === id;
}

function isEditingClassName() {
  const ae = document.activeElement;
  if (!ae || ae.tagName !== "INPUT" || ae.type !== "text") return false;
  return !!ae.closest("#targetGrid");
}

function isEditingXhairPresetName() {
  return isFocusedId("xhairPresetName");
}

function isEditingStreamerField() {
  const ae = document.activeElement;
  return !!ae && !!ae.closest && !!ae.closest("#tab-system") && String(ae.id || "").startsWith("stream");
}

function setValIfIdle(id, value) {
  const el = document.getElementById(id);
  if (!el || isFocusedId(id)) return;
  el.value = value;
}

function sendVal(k, v) {
  const n = Number(v);
  appState.params[k] = n;
  const el = document.getElementById(k + "_val");
  if (el) el.innerText = fmt(k, n);
  fetch("/api/update", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ [k]: n }),
  });
}

function setStreamerSizePreset(value) {
  if (value === "custom") return;
  const n = Number(value);
  if (!Number.isFinite(n)) return;
  const w = document.getElementById("streamWidth");
  const h = document.getElementById("streamHeight");
  if (w) w.value = n;
  if (h) h.value = n;
  scheduleStreamerConfigSave();
}

function setStreamerProtocol(protocol, shouldSave = true) {
  const value = protocol === "tcp" ? "tcp" : "udp";
  const hidden = document.getElementById("streamProtocol");
  if (hidden) hidden.value = value;
  const label = document.getElementById("streamProtocolLabel");
  if (label) label.innerText = value.toUpperCase();
  ["udp", "tcp"].forEach((name) => {
    const btn = document.getElementById(`streamProtocol${name.toUpperCase()}`);
    if (btn) btn.classList.toggle("active", name === value);
  });
  const pkt = document.getElementById("streamPktSize");
  if (pkt) pkt.disabled = value === "tcp";
  const pktWrap = document.getElementById("streamPktSizeWrap");
  if (pktWrap) pktWrap.classList.toggle("field-disabled", value === "tcp");
  if (shouldSave) scheduleStreamerConfigSave();
}

function streamerPayloadFromForm() {
  const get = (id, fallback) => {
    const el = document.getElementById(id);
    if (!el) return fallback;
    return el.value === "" ? fallback : el.value;
  };
  return {
    host: String(get("streamHost", "192.168.3.113")).trim(),
    port: Number(get("streamPort", 9999)),
    protocol: String(get("streamProtocol", "udp")).toLowerCase() === "tcp" ? "tcp" : "udp",
    width: Number(get("streamWidth", 416)),
    height: Number(get("streamHeight", 416)),
    fps: Number(get("streamFps", 240)),
    quality: Number(get("streamQuality", 85)),
    pkt_size: Number(get("streamPktSize", 1316)),
  };
}

function renderStreamer(streamer) {
  streamer = streamer || {};
  const cfg = streamer.config || {};
  const recentlyEdited = streamerConfigDirty || Date.now() - streamerLastEditAt < 1800;
  const protocolInput = document.getElementById("streamProtocol");
  const formProtocol = recentlyEdited ? ((protocolInput && protocolInput.value) || cfg.protocol || "udp") : (cfg.protocol || "udp");
  setStreamerProtocol(formProtocol, false);
  if (!isEditingStreamerField() && !recentlyEdited) {
    setValIfIdle("streamHost", cfg.host ?? "192.168.3.113");
    setValIfIdle("streamPort", cfg.port ?? 9999);
    setValIfIdle("streamWidth", cfg.width ?? 416);
    setValIfIdle("streamHeight", cfg.height ?? 416);
    setValIfIdle("streamFps", cfg.fps ?? 240);
    setValIfIdle("streamQuality", cfg.quality ?? 85);
    setValIfIdle("streamPktSize", cfg.pkt_size ?? 1316);
    const preset = document.getElementById("streamSizePreset");
    if (preset && !isFocusedId("streamSizePreset")) {
      const w = Number(cfg.width ?? 416);
      const h = Number(cfg.height ?? 416);
      preset.value = w === h && [256, 320, 416, 640].includes(w) ? String(w) : "custom";
    }
  }

  const running = !!streamer.running;
  const status = document.getElementById("streamerStatus");
  if (status) {
    if (running) {
      status.innerText = "发射中";
      status.style.color = "#15803d";
    } else if (streamer.error) {
      status.innerText = "异常停止";
      status.style.color = "#b91c1c";
    } else {
      status.innerText = "未启动";
      status.style.color = "#6b7a88";
    }
  }

  const fps = document.getElementById("streamerFps");
  if (fps) fps.innerText = Number(streamer.fps || 0).toFixed(1);
  const frame = document.getElementById("streamerFrame");
  if (frame) frame.innerText = String(streamer.frame || 0);
  const path = document.getElementById("streamerLogPath");
  if (path) path.innerText = streamer.log_path || "-";
  const startBtn = document.getElementById("streamStartBtn");
  if (startBtn) startBtn.disabled = running;
  const stopBtn = document.getElementById("streamStopBtn");
  if (stopBtn) stopBtn.disabled = !running;
}

function scheduleStreamerConfigSave() {
  streamerConfigDirty = true;
  streamerLastEditAt = Date.now();
  if (streamerSaveTimer) clearTimeout(streamerSaveTimer);
  streamerSaveTimer = setTimeout(saveStreamerConfig, 450);
}

function saveStreamerConfig() {
  const payload = streamerPayloadFromForm();
  fetch("/api/streamer/config", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) {
        const status = document.getElementById("streamerStatus");
        if (status) {
          status.innerText = "配置保存失败";
          status.style.color = "#b91c1c";
        }
        return;
      }
      streamerConfigDirty = false;
      if (d.streamer) renderStreamer(d.streamer);
    })
    .catch(() => {
      const status = document.getElementById("streamerStatus");
      if (status) {
        status.innerText = "配置接口不可用";
        status.style.color = "#b91c1c";
      }
    });
}

function initStreamerFormEvents() {
  ["streamHost", "streamPort", "streamWidth", "streamHeight", "streamFps", "streamQuality", "streamPktSize"].forEach((id) => {
    const el = document.getElementById(id);
    if (!el) return;
    el.addEventListener("input", scheduleStreamerConfigSave);
    el.addEventListener("change", scheduleStreamerConfigSave);
  });
}

function sendPreviewEnabled(checked) {
  const v = checked ? 1 : 0;
  sendVal("preview_en", v);
  const a = document.getElementById("preview_en");
  const b = document.getElementById("xhair_preview_en");
  if (a) a.checked = checked;
  if (b) b.checked = checked;
}

function sendPreviewFps(value) {
  sendVal("preview_fps", value);
  const n = Number(value);
  const a = document.getElementById("preview_fps");
  const b = document.getElementById("xhair_preview_fps");
  const av = document.getElementById("preview_fps_val");
  const bv = document.getElementById("xhair_preview_fps_val");
  if (a) a.value = n;
  if (b) b.value = n;
  if (av) av.innerText = fmt("preview_fps", n);
  if (bv) bv.innerText = fmt("preview_fps", n);
}

function setFamily(name, markDirty = true) {
  selectedFamily = name;
  if (markDirty) {
    familyDirty = true;
    pendingFamily = name;
  }
  ["yolov8", "yolov11", "yolov26"].forEach((x) => {
    const el = document.getElementById("family_" + x);
    if (el) el.classList.toggle("active", x === name);
  });
}

function setNpuMode(name, markDirty = true) {
  selectedNpuMode = name === "throughput" ? "throughput" : "latency";
  if (markDirty) {
    npuModeDirty = true;
    pendingNpuMode = selectedNpuMode;
  }
  ["latency", "throughput"].forEach((x) => {
    const el = document.getElementById("npu_" + x);
    if (el) el.classList.toggle("active", x === selectedNpuMode);
  });
  renderNpuModeStatus();
}

function renderNpuModeStatus() {
  const el = document.getElementById("npuModeStatus");
  if (!el) return;
  const runtimeMode = appState.runtime.npu_mode || "latency";
  const label = (mode) => (mode === "throughput" ? "高吞吐三核" : "低延迟单核");
  el.innerText = npuModeDirty
    ? `待应用：${label(selectedNpuMode)}，当前：${label(runtimeMode)}`
    : label(runtimeMode);
}

function onModelSelectChanged(value) {
  modelSelectDirty = true;
  pendingModelId = value;
}

function onConfigSelectChanged(value) {
  configSelectDirty = true;
  pendingConfigName = value;
}

function renderModelSelect() {
  const s = document.getElementById("modelSelect");
  if (!s) return;
  s.innerHTML = "";
  const runtimeId = appState.runtime.selected_model_id;
  const desiredId = modelSelectDirty && pendingModelId ? pendingModelId : runtimeId;
  let hasDesired = false;
  (appState.models || []).forEach((m) => {
    const o = document.createElement("option");
    o.value = m.id;
    o.textContent = m.name;
    if (m.id === desiredId) {
      o.selected = true;
      hasDesired = true;
    }
    s.appendChild(o);
  });
  if (!hasDesired && runtimeId) {
    for (let i = 0; i < s.options.length; i++) {
      if (s.options[i].value === runtimeId) {
        s.options[i].selected = true;
        break;
      }
    }
  }
  if (!hasDesired && s.options.length > 0 && !s.value) s.options[0].selected = true;
}

function renderConfigSelect() {
  const s = document.getElementById("configSelect");
  if (!s) return;
  s.innerHTML = "";
  const runtimeName = appState.runtime.active_config || "default";
  const desiredName = configSelectDirty && pendingConfigName ? pendingConfigName : runtimeName;
  let hasDesired = false;
  (appState.profiles || []).forEach((name) => {
    const o = document.createElement("option");
    o.value = name;
    o.textContent = name + ".json";
    if (name === desiredName) {
      o.selected = true;
      hasDesired = true;
    }
    s.appendChild(o);
  });
  if (!hasDesired && runtimeName) {
    for (let i = 0; i < s.options.length; i++) {
      if (s.options[i].value === runtimeName) {
        s.options[i].selected = true;
        break;
      }
    }
  }
  if (!hasDesired && s.options.length > 0 && !s.value) s.options[0].selected = true;
  const active = document.getElementById("activeConfigName");
  if (active) active.innerText = (appState.runtime.active_config || "default") + ".json";
}

function clampNum(v, lo, hi, fallback = 0) {
  const n = Number(v);
  if (!Number.isFinite(n)) return fallback;
  return Math.max(lo, Math.min(hi, Math.round(n)));
}

function hsvToRgbCss(h, s, v) {
  h = clampNum(h, 0, 180, 0) * 2;
  s = clampNum(s, 0, 255, 0) / 255;
  v = clampNum(v, 0, 255, 0) / 255;
  const c = v * s;
  const x = c * (1 - Math.abs(((h / 60) % 2) - 1));
  const m = v - c;
  let r = 0, g = 0, b = 0;
  if (h < 60) [r, g, b] = [c, x, 0];
  else if (h < 120) [r, g, b] = [x, c, 0];
  else if (h < 180) [r, g, b] = [0, c, x];
  else if (h < 240) [r, g, b] = [0, x, c];
  else if (h < 300) [r, g, b] = [x, 0, c];
  else [r, g, b] = [c, 0, x];
  return `rgb(${Math.round((r + m) * 255)}, ${Math.round((g + m) * 255)}, ${Math.round((b + m) * 255)})`;
}

function getXhairHsvParams() {
  return {
    hMin: clampNum(appState.params.xhair_h_min ?? 170, 0, 180, 170),
    hMax: clampNum(appState.params.xhair_h_max ?? 10, 0, 180, 10),
    sMin: clampNum(appState.params.xhair_s_min ?? 120, 0, 255, 120),
    sMax: clampNum(appState.params.xhair_s_max ?? 255, 0, 255, 255),
    vMin: clampNum(appState.params.xhair_v_min ?? 120, 0, 255, 120),
    vMax: clampNum(appState.params.xhair_v_max ?? 255, 0, 255, 255),
  };
}

function updateXhairSummary() {
  const p = getXhairHsvParams();
  const summary = document.getElementById("xhairHsvSummary");
  if (summary) {
    summary.innerText = `H ${p.hMin}-${p.hMax} / S ${p.sMin}-${p.sMax} / V ${p.vMin}-${p.vMax}`;
  }
  const chip = document.getElementById("xhairColorChip");
  if (chip) {
    const hue = p.hMin > p.hMax ? 0 : Math.round((p.hMin + p.hMax) / 2);
    chip.style.background = hsvToRgbCss(hue, p.sMax, p.vMax);
  }
}

function renderXhairPresets() {
  const select = document.getElementById("xhairPresetSelect");
  if (!select) return;
  const presets = appState.runtime.xhair_presets || [];
  const selected = appState.runtime.xhair_selected_preset || (presets[0] || {}).name || "";
  if (!isFocusedId("xhairPresetSelect")) {
    select.innerHTML = "";
    presets.forEach((preset) => {
      const o = document.createElement("option");
      o.value = preset.name;
      o.textContent = preset.name;
      if (String(preset.name).toLowerCase() === String(selected).toLowerCase()) {
        o.selected = true;
      }
      select.appendChild(o);
    });
  }
  const nameInput = document.getElementById("xhairPresetName");
  if (nameInput && !isEditingXhairPresetName()) {
    nameInput.value = selected;
  }
  updateXhairSummary();
}

function markXhairCustom() {
  updateXhairSummary();
  const nameInput = document.getElementById("xhairPresetName");
  if (nameInput && !nameInput.value.trim()) {
    nameInput.value = "Custom";
  }
}

function selectXhairPreset(name) {
  if (!name) return;
  fetch("/api/crosshair_preset/select", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ name }),
  })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) {
        alert("选择准心颜色失败: " + d.msg);
        return;
      }
      applyState(d.state);
    });
}

function saveXhairPreset() {
  const input = document.getElementById("xhairPresetName");
  const name = (input ? input.value : "").trim();
  if (!name) {
    alert("请输入准心颜色名称");
    return;
  }
  fetch("/api/crosshair_preset/save", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ name }),
  })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) {
        alert("保存准心颜色失败: " + d.msg);
        return;
      }
      applyState(d.state);
    });
}

function deleteXhairPreset() {
  const select = document.getElementById("xhairPresetSelect");
  const name = select ? select.value : "";
  if (!name) return;
  if (!confirm("删除准心颜色预设: " + name + " ?")) return;
  fetch("/api/crosshair_preset/delete", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ name }),
  })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) {
        alert("删除准心颜色失败: " + d.msg);
        return;
      }
      applyState(d.state);
    });
}

function renderTargets() {
  const g = document.getElementById("targetGrid");
  if (!g) return;
  g.innerHTML = "";
  const c = Number(appState.runtime.num_classes || 1);
  for (let i = 0; i < c; i++) {
    const en = `c${i}_en`;
    const hr = `c${i}_hr`;
    const py = `c${i}_py`;
    const pri = `c${i}_pri`;
    const enV = Number(appState.params[en] ?? 1);
    const hrV = Number(appState.params[hr] ?? 0.3);
    const pyV = Number(appState.params[py] ?? 0);
    const priV = Number(appState.params[pri] ?? (i === 1 || i === 7 ? 0 : 10));
    const className = escapeHtml(getClassName(i));

    const d = document.createElement("div");
    d.className = "target";
    d.innerHTML = `
      <div class="target-h">
        <span>类别 ${i}</span>
        <input style="width:auto;" type="checkbox" ${enV > 0.5 ? "checked" : ""} onchange="sendVal('${en}', this.checked ? 1 : 0)">
      </div>
      <div class="row"><span>类别名称</span></div>
      <input class="class-name-input" type="text" value="${className}" onblur="updateClassName(${i}, this.value)" onkeydown="if(event.key==='Enter'){this.blur();}">
      <div class="row"><span>高度比例%</span><span class="val" id="${hr}_val">${hrV.toFixed(2)}</span></div>
      <input type="range" min="0" max="1" step="0.05" value="${hrV}" oninput="sendVal('${hr}', this.value)">
      <div class="row"><span>像素微调px</span><span class="val" id="${py}_val">${pyV.toFixed(0)}</span></div>
      <input type="range" min="-150" max="150" step="1" value="${pyV}" oninput="sendVal('${py}', this.value)">
      <div class="row"><span>&#30644;&#20934;&#20248;&#20808;&#32423;</span><span class="val" id="${pri}_val">${priV.toFixed(0)}</span></div>
      <input type="range" min="0" max="99" step="1" value="${priV}" oninput="sendVal('${pri}', this.value)">
    `;
    g.appendChild(d);
  }
}

function applyState(d) {
  appState = d;
  if (!familyDirty) {
    selectedFamily = d.runtime.model_family || "yolov8";
    setFamily(selectedFamily, false);
  } else {
    if (pendingFamily !== selectedFamily) pendingFamily = selectedFamily;
    setFamily(selectedFamily, false);
  }
  if (!npuModeDirty) {
    selectedNpuMode = d.runtime.npu_mode || "latency";
    setNpuMode(selectedNpuMode, false);
  } else {
    if (pendingNpuMode !== selectedNpuMode) pendingNpuMode = selectedNpuMode;
    setNpuMode(selectedNpuMode, false);
  }

  const currentModel = document.getElementById("currentModelName");
  if (currentModel) currentModel.innerText = d.current_model_name || "-";
  const clsCount = document.getElementById("targetClassCount");
  if (clsCount) clsCount.innerText = String(d.runtime.num_classes || 1);

  if (!isFocusedId("modelSelect")) renderModelSelect();
  if (!isFocusedId("configSelect")) renderConfigSelect();
  if (!isEditingClassName()) renderTargets();
  renderXhairPresets();
  renderAimMask(getAimMask());
  renderTriggerMask(getTriggerMask());
  renderStreamer(d.streamer || {});

  [
    "kp_x",
    "kd_x",
    "kp_y",
    "kd_y",
    "recoil",
    "max_step",
    "deadzone",
    "aim_slow_radius",
    "aim_slow_min_scale",
    "pred_lead_ms",
    "pred_vel_smooth",
    "pred_pos_smooth",
    "pred_max_speed",
    "conf_thres",
    "nms_iou",
    "aim_fov",
    "trigger_radius",
    "trigger_delay_ms",
    "preview_fps",
    "xhair_c",
    "xhair_h_min",
    "xhair_h_max",
    "xhair_s_min",
    "xhair_s_max",
    "xhair_v_min",
    "xhair_v_max",
  ].forEach((k) => {
    const i = document.getElementById(k);
    if (i) i.value = d.params[k] ?? 0;
    const v = document.getElementById(k + "_val");
    if (v) v.innerText = fmt(k, d.params[k] ?? 0);
  });

  const pe = document.getElementById("pred_en");
  if (pe) pe.checked = Number(d.params.pred_en ?? 0) > 0.5;
  const xe = document.getElementById("xhair_en");
  if (xe) xe.checked = Number(d.params.xhair_en ?? 0) > 0.5;
  const pv = document.getElementById("preview_en");
  if (pv) pv.checked = Number(d.params.preview_en ?? 0) > 0.5;
  const xpv = document.getElementById("xhair_preview_en");
  if (xpv) xpv.checked = Number(d.params.preview_en ?? 0) > 0.5;
  const xpf = document.getElementById("xhair_preview_fps");
  if (xpf) xpf.value = d.params.preview_fps ?? 20;
  const xpfv = document.getElementById("xhair_preview_fps_val");
  if (xpfv) xpfv.innerText = fmt("preview_fps", d.params.preview_fps ?? 20);

  const b = document.getElementById("toggleBtn");
  if (b) {
    if (d.engine_running) {
      b.className = "btn-toggle btn-on";
      b.innerText = "引擎运行中（点击停止）";
    } else {
      b.className = "btn-toggle btn-off";
      b.innerText = "引擎已停止（点击启动）";
    }
  }

  const teleFps = document.getElementById("tele_fps");
  if (teleFps) teleFps.innerText = d.telemetry.fps;
  const teleTime = document.getElementById("tele_time");
  if (teleTime) teleTime.innerText = Number(d.telemetry.infer_time).toFixed(1);
  const sysCpu = document.getElementById("sys_cpu");
  if (sysCpu) sysCpu.innerText = Number((d.system || {}).cpu ?? 0).toFixed(1);
  const npu = Number((d.system || {}).npu ?? -1);
  const sysNpu = document.getElementById("sys_npu");
  if (sysNpu) sysNpu.innerText = npu >= 0 ? npu.toFixed(1) + "%" : "--";
  const sysMem = document.getElementById("sys_mem");
  if (sysMem) sysMem.innerText = Number((d.system || {}).mem ?? 0).toFixed(1);
  const sysDisk = document.getElementById("sys_disk");
  if (sysDisk) sysDisk.innerText = Number((d.system || {}).disk ?? 0).toFixed(1);

  const st = document.getElementById("tele_status");
  if (st) {
    st.innerText = d.telemetry.is_aiming ? "LOCK" : "IDLE";
    st.style.color = d.telemetry.is_aiming ? "#b91c1c" : "#6b7a88";
  }

  const tg = document.getElementById("tele_target");
  if (tg) {
    if (Number(d.telemetry.cls) >= 0) {
      const cls = Number(d.telemetry.cls);
      tg.innerText = `${getClassName(cls)}(${Number(d.telemetry.conf).toFixed(2)})`;
      tg.style.color = "#b91c1c";
    } else {
      tg.innerText = "Safe";
      tg.style.color = "#6b7a88";
    }
  }

  const ee = document.getElementById("tele_engine_err");
  if (ee) {
    if (d.engine_running) {
      ee.innerText = "运行中";
      ee.style.color = "#0b67d0";
    } else if (d.engine_error) {
      const msg = String(d.engine_error).split("\n")[0];
      ee.innerText = msg.length > 60 ? msg.slice(0, 60) + "..." : msg;
      ee.style.color = "#b91c1c";
    } else {
      ee.innerText = "已停止";
      ee.style.color = "#6b7a88";
    }
  }
}

function fetchState() {
  fetch("/api/state")
    .then((r) => r.json())
    .then(applyState)
    .catch(() => {});
}

function fetchLogs(forceScroll = false) {
  fetch("/api/logs?tail=220")
    .then((r) => r.json())
    .then((d) => {
      const box = document.getElementById("engineConsole");
      const path = document.getElementById("engineLogPath");
      if (path) path.innerText = d.path || "-";
      if (!box) return;
      if (!d.ok) {
        box.innerText = "读取日志失败: " + (d.msg || "未知错误");
        return;
      }
      const text = d.log && String(d.log).trim() ? String(d.log) : "暂无日志（引擎未启动或尚未输出）";
      const isNearBottom = box.scrollTop + box.clientHeight + 40 >= box.scrollHeight;
      box.textContent = text;
      if (forceScroll || isNearBottom) box.scrollTop = box.scrollHeight;
    })
    .catch(() => {
      const box = document.getElementById("engineConsole");
      if (box) box.innerText = "日志接口不可用";
    });
}

function fetchStreamerLogs(forceScroll = false) {
  fetch("/api/streamer/logs?tail=120")
    .then((r) => r.json())
    .then((d) => {
      const box = document.getElementById("streamerConsole");
      const path = document.getElementById("streamerLogPath");
      if (path) path.innerText = d.path || "-";
      if (!box) return;
      const text = d.log && String(d.log).trim() ? String(d.log) : "暂无发射器日志";
      const isNearBottom = box.scrollTop + box.clientHeight + 40 >= box.scrollHeight;
      box.textContent = text;
      if (forceScroll || isNearBottom) box.scrollTop = box.scrollHeight;
    })
    .catch(() => {
      const box = document.getElementById("streamerConsole");
      if (box) box.innerText = "发射器日志接口不可用";
    });
}

function startStreamer() {
  const payload = streamerPayloadFromForm();
  fetch("/api/streamer/start", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) alert("发射器启动失败: " + (d.msg || "未知错误"));
      if (d.streamer) renderStreamer(d.streamer);
      fetchStreamerLogs(true);
      fetchState();
    })
    .catch(() => alert("发射器启动接口不可用"));
}

function stopStreamer() {
  fetch("/api/streamer/stop", { method: "POST" })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) alert("停止发射器失败: " + (d.msg || "未知错误"));
      if (d.streamer) renderStreamer(d.streamer);
      fetchStreamerLogs(true);
      fetchState();
    })
    .catch(() => alert("发射器停止接口不可用"));
}

function toggleEngine() {
  fetch("/api/toggle", { method: "POST" })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) alert("启动失败: " + (d.msg || "未知错误"));
      fetchLogs(true);
      fetchState();
    })
    .catch(() => {
      fetchLogs(false);
      fetchState();
    });
}

function applyModelConfig() {
  const modelEl = document.getElementById("modelSelect");
  const modelId = modelSelectDirty && pendingModelId ? pendingModelId : modelEl.value;
  const payload = { selected_model_id: modelId, model_family: selectedFamily, npu_mode: selectedNpuMode };
  fetch("/api/runtime", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) {
        alert("应用失败: " + d.msg);
        return;
      }
      modelSelectDirty = false;
      pendingModelId = null;
      familyDirty = false;
      pendingFamily = null;
      npuModeDirty = false;
      pendingNpuMode = null;
      renderNpuModeStatus();
      fetchState();
    });
}

function changeClassCount(delta) {
  const current = Number(appState.runtime.num_classes || 1);
  const next = Math.max(1, Math.min(100, current + delta));
  if (next === current) return;
  const payload = {
    selected_model_id: document.getElementById("modelSelect").value,
    model_family: selectedFamily,
    npu_mode: selectedNpuMode,
    num_classes: next,
  };
  fetch("/api/runtime", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) {
        alert("调整类别数失败: " + d.msg);
        return;
      }
      fetchState();
    });
}

function updateClassName(index, name) {
  fetch("/api/class_name", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ index: index, name: name }),
  })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) {
        alert("保存类别名称失败: " + d.msg);
        return;
      }
      fetchState();
    });
}

function uploadModel() {
  const fi = document.getElementById("modelFileInput");
  if (!fi.files || fi.files.length === 0) {
    alert("请先选择 .rknn 文件");
    return;
  }
  const fd = new FormData();
  fd.append("model_file", fi.files[0]);
  fetch("/api/upload_model", { method: "POST", body: fd })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) {
        alert("上传失败: " + d.msg);
        return;
      }
      alert("上传成功: " + d.filename);
      fi.value = "";
      fetchState();
    });
}

function switchConfig() {
  const s = document.getElementById("configSelect");
  const name = configSelectDirty && pendingConfigName ? pendingConfigName : s.value;
  fetch("/api/config/switch", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ config_name: name }),
  })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) {
        alert("切换失败: " + d.msg);
        return;
      }
      configSelectDirty = false;
      pendingConfigName = null;
      modelSelectDirty = false;
      pendingModelId = null;
      familyDirty = false;
      pendingFamily = null;
      npuModeDirty = false;
      pendingNpuMode = null;
      fetchState();
    });
}

function saveConfig() {
  fetch("/api/config/save", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({}),
  })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) {
        alert("保存失败: " + d.msg);
        return;
      }
      fetchState();
    });
}

function createConfig() {
  const name = (document.getElementById("newConfigName").value || "").trim();
  if (!name) {
    alert("请输入配置名称");
    return;
  }
  fetch("/api/config/create", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ config_name: name }),
  })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) {
        alert("创建失败: " + d.msg);
        return;
      }
      document.getElementById("newConfigName").value = "";
      configSelectDirty = false;
      pendingConfigName = null;
      npuModeDirty = false;
      pendingNpuMode = null;
      fetchState();
    });
}

function systemAction(action) {
  if (action === "edid") {
    if (!confirm("确认重新刷入 240Hz EDID 吗？")) return;
  } else {
    const txt = action === "shutdown" ? "关机" : "重启";
    if (!confirm("确认要" + txt + "设备吗？")) return;
  }
  fetch("/api/system/" + action, { method: "POST" })
    .then((r) => r.json())
    .then((d) => {
      if (!d.ok) alert("操作失败: " + d.msg);
      else if (action === "edid") alert("240Hz EDID 已重新刷入");
    });
}

function refreshPreview() {
  const enabled = Number(appState.params.preview_en ?? 0) > 0.5;
  const pairs = [
    ["previewImg", "previewHint"],
    ["xhairPreviewImg", "xhairPreviewHint"],
  ];
  const setPair = (imgId, hintId, src = null) => {
    const img = document.getElementById(imgId);
    const hint = document.getElementById(hintId);
    if (!img || !hint) return;
    if (!enabled) {
      img.style.display = "none";
      hint.style.display = "flex";
      return;
    }
    hint.style.display = "none";
    img.style.display = "block";
    if (src) img.src = src;
  };
  if (!enabled) {
    lastPreviewRefreshAt = 0;
    pairs.forEach(([imgId, hintId]) => setPair(imgId, hintId));
    return;
  }
  const fps = Math.max(5, Math.min(30, Number(appState.params.preview_fps ?? 20)));
  const now = Date.now();
  if (now - lastPreviewRefreshAt < 1000 / fps) return;
  lastPreviewRefreshAt = now;
  const src = "/static/preview.jpg?t=" + now;
  pairs.forEach(([imgId, hintId]) => setPair(imgId, hintId, src));
}

const uz = document.getElementById("uploadZone");
if (uz) {
  uz.addEventListener("dragover", (e) => {
    e.preventDefault();
    uz.style.background = "#d8f6ef";
  });
  uz.addEventListener("dragleave", () => {
    uz.style.background = "";
  });
  uz.addEventListener("drop", (e) => {
    e.preventDefault();
    uz.style.background = "";
    const files = e.dataTransfer.files;
    if (files && files.length > 0) {
      document.getElementById("modelFileInput").files = files;
    }
  });
}

setInterval(fetchState, 700);
setInterval(fetchLogs, 1500);
setInterval(fetchStreamerLogs, 1500);
setInterval(refreshPreview, 33);
initStreamerFormEvents();
fetchState();
fetchLogs(true);
fetchStreamerLogs(true);
refreshPreview();
