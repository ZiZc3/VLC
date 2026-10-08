/*
 * The page VLC-PS5 serves to phones and computers (see web.cc): send files
 * and a remote. One file, no outside resources (the console may have no
 * internet), sized for a phone first.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

static const char WEB_PAGE[] = R"VLCPAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#0b0b0f">
<title>VLC for PS5</title>
<style>
:root {
  --bg: #0b0b0f; --panel: #16161d; --panel2: #1e1e27; --line: #2a2a35;
  --fg: #f4f4f7; --dim: #9a9aa8; --faint: #62626f; --accent: #ff8800; --accent2: #ff5e14;
  --ok: #3fd6a4; --bad: #ff5d6c;
  --sans: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
}
* { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
html, body { margin: 0; background: var(--bg); color: var(--fg); font-family: var(--sans); }
body { padding: env(safe-area-inset-top, 0) 16px calc(24px + env(safe-area-inset-bottom, 0)); max-width: 640px; margin: 0 auto; }
header { display: flex; align-items: center; gap: 12px; padding: 20px 0 16px; }
header svg { width: 34px; height: 34px; flex: none; }
header h1 { font-size: 20px; margin: 0; letter-spacing: -0.01em; }
header p { margin: 2px 0 0; color: var(--dim); font-size: 13px; }
.tabs { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; background: var(--panel); padding: 5px; border-radius: 12px; }
.tabs button { font: inherit; font-weight: 600; font-size: 15px; color: var(--dim); background: none; border: 0; padding: 11px; border-radius: 9px; cursor: pointer; }
.tabs button[aria-pressed="true"] { background: var(--accent); color: #140900; }
section { margin-top: 18px; display: grid; gap: 14px; }
.card { background: var(--panel); border: 1px solid var(--line); border-radius: 16px; padding: 16px; display: grid; gap: 12px; }
label.small { font-size: 12px; color: var(--faint); text-transform: uppercase; letter-spacing: .08em; }
select { font: inherit; color: var(--fg); background: var(--panel2); border: 1px solid var(--line); border-radius: 10px; padding: 11px 12px; width: 100%; }
.drop { display: grid; place-items: center; gap: 6px; text-align: center; padding: 30px 16px; border: 2px dashed var(--line); border-radius: 14px; cursor: pointer; }
.drop.over { border-color: var(--accent); background: rgba(255,136,0,.06); }
.drop b { font-size: 17px; }
.drop span { color: var(--dim); font-size: 13px; }
.drop input { display: none; }
.big { font: inherit; font-weight: 700; font-size: 16px; color: #140900; background: var(--accent); border: 0; border-radius: 12px; padding: 13px 18px; cursor: pointer; }
.list { display: grid; gap: 8px; }
.item { display: grid; grid-template-columns: minmax(0,1fr) auto; gap: 4px 10px; align-items: center; background: var(--panel2); border-radius: 12px; padding: 10px 12px; }
.item .name { font-size: 14px; font-weight: 600; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.item .meta { grid-column: 1; font-size: 12px; color: var(--dim); font-variant-numeric: tabular-nums; }
.item .bar { grid-column: 1 / -1; height: 5px; background: var(--line); border-radius: 3px; overflow: hidden; }
.item .bar i { display: block; height: 100%; width: 0; background: linear-gradient(90deg, var(--accent2), var(--accent)); }
.item.done .bar i { background: var(--ok); }
.item.fail .bar i { background: var(--bad); }
.del { font: inherit; font-size: 13px; color: var(--dim); background: none; border: 1px solid var(--line); border-radius: 8px; padding: 6px 10px; cursor: pointer; grid-row: 1 / span 2; grid-column: 2; }
.empty { color: var(--faint); font-size: 14px; text-align: center; padding: 10px; }
.now { display: grid; gap: 6px; }
.now h2 { margin: 0; font-size: 18px; overflow-wrap: anywhere; }
.now p { margin: 0; color: var(--dim); font-size: 13px; }
input[type=range] { width: 100%; accent-color: var(--accent); }
.times { display: flex; justify-content: space-between; color: var(--dim); font-size: 12px; font-variant-numeric: tabular-nums; }
.pad { display: grid; grid-template-columns: repeat(5, 1fr); gap: 8px; }
.pad button, .row button { font: inherit; font-weight: 700; color: var(--fg); background: var(--panel2); border: 1px solid var(--line); border-radius: 14px; aspect-ratio: 1; cursor: pointer; display: grid; place-items: center; }
.pad button svg { width: 26px; height: 26px; fill: currentColor; }
.pad button.main { background: var(--fg); color: var(--bg); }
.row { display: grid; grid-template-columns: 1fr auto 1fr auto; gap: 8px; align-items: center; }
.row button { aspect-ratio: auto; padding: 12px; font-size: 18px; }
.row span { text-align: center; font-variant-numeric: tabular-nums; color: var(--dim); }
.field { font: inherit; color: var(--fg); background: var(--panel2); border: 1px solid var(--line); border-radius: 10px; padding: 11px 12px; width: 100%; }
.note { margin: 0; color: var(--dim); font-size: 13px; }
.note a { color: var(--accent); }
button:active { transform: scale(.97); }
button:focus-visible, select:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
footer { margin-top: 22px; color: var(--faint); font-size: 12px; text-align: center; }
</style>
</head>
<body>
<header>
  <svg viewBox="0 0 64 64" aria-hidden="true"><path d="M32 4 9 56h46z" fill="#ff8800"/><path d="M24 22h16l2.6 6H21.4zM18.6 34h26.8l2.6 6H16z" fill="#fff"/><rect x="5" y="54" width="54" height="7" rx="2.5" fill="#ff5e14"/></svg>
  <div><h1>VLC for PS5</h1><p id="who">On your console</p></div>
</header>

<div class="tabs" role="tablist">
  <button id="t-send" aria-pressed="true">Send files</button>
  <button id="t-remote" aria-pressed="false">Remote</button>
</div>

<section id="p-send">
  <div class="card">
    <label class="small" for="place">Save to</label>
    <select id="place"></select>
    <label class="drop" id="drop">
      <input id="pick" type="file" multiple accept="video/*,audio/*,.mkv,.avi,.ts,.m2ts,.webm,.flac,.opus,.srt,.ass,.ssa,.vtt,.m3u,.m3u8,.pls">
      <b>Choose videos or music</b>
      <span>or drop them here. Subtitles (.srt, .ass) too.</span>
    </label>
    <div class="list" id="queue"></div>
  </div>
  <div class="card">
    <label class="small">In this folder</label>
    <div class="list" id="files"><div class="empty">Loading…</div></div>
  </div>
  <div class="card">
    <label class="small" for="os-key">Subtitle downloads (OpenSubtitles)</label>
    <p class="note">Paste your own free API key from opensubtitles.com (Profile › API consumers). An account is optional and gives more downloads a day.</p>
    <input class="field" id="os-key" placeholder="API key" autocomplete="off" autocapitalize="off" spellcheck="false">
    <input class="field" id="os-user" placeholder="User name (optional)" autocomplete="username" autocapitalize="off">
    <input class="field" id="os-pass" type="password" placeholder="Password (optional)" autocomplete="current-password">
    <button class="big" id="os-save">Save on the PS5</button>
    <p class="note" id="os-msg"></p>
  </div>
</section>

<section id="p-remote" hidden>
  <div class="card now">
    <h2 id="title">Nothing playing</h2>
    <p id="state">Start a video on the console</p>
    <input id="seek" type="range" min="0" max="1000" value="0" aria-label="Position">
    <div class="times"><span id="t-now">0:00</span><span id="t-len">0:00</span></div>
  </div>
  <div class="pad">
    <button data-c="prev" aria-label="Previous"><svg viewBox="0 0 24 24"><path d="M6 5h2v14H6zM9.5 12 19 5v14z"/></svg></button>
    <button data-c="jump" data-v="-10000" aria-label="Back 10 seconds"><svg viewBox="0 0 24 24"><path d="M11 6 3 12l8 6zM20 6l-8 6 8 6z"/></svg></button>
    <button data-c="toggle" class="main" aria-label="Play or pause"><svg viewBox="0 0 24 24" id="pp"><path d="M8 5v14l11-7z"/></svg></button>
    <button data-c="jump" data-v="10000" aria-label="Forward 10 seconds"><svg viewBox="0 0 24 24"><path d="m13 6 8 6-8 6zM4 6l8 6-8 6z"/></svg></button>
    <button data-c="next" aria-label="Next"><svg viewBox="0 0 24 24"><path d="M16 5h2v14h-2zM14.5 12 5 5v14z"/></svg></button>
  </div>
  <div class="row">
    <button data-c="vol" data-v="-10" aria-label="Volume down">−</button>
    <span id="vol">Volume 100%</span>
    <button data-c="vol" data-v="10" aria-label="Volume up">+</button>
    <button data-c="stop" aria-label="Stop">Stop</button>
  </div>
</section>

<footer>Works on the same Wi-Fi as the PS5. Nothing leaves your home network.</footer>

<script>
const $ = s => document.querySelector(s);
const fmtSize = b => b > 1e9 ? (b / 1e9).toFixed(2) + " GB" : (b / 1e6).toFixed(1) + " MB";
const fmtTime = ms => { const s = Math.max(0, Math.floor(ms / 1000)), h = Math.floor(s / 3600),
  m = Math.floor(s / 60) % 60, x = String(s % 60).padStart(2, "0");
  return h ? h + ":" + String(m).padStart(2, "0") + ":" + x : m + ":" + x; };
const esc = t => t.replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);

/* OpenSubtitles: the key and account go to the console, which keeps them. */
$("#os-save").onclick = async () => {
  const body = "key=" + encodeURIComponent($("#os-key").value.trim()) +
    "&user=" + encodeURIComponent($("#os-user").value.trim()) +
    "&pass=" + encodeURIComponent($("#os-pass").value);
  const r = await fetch("/api/opensubtitles", { method: "POST", body,
    headers: { "Content-Type": "application/x-www-form-urlencoded" } }).catch(() => null);
  $("#os-msg").textContent = r && r.ok ? "Saved. The PS5 checks it and shows the result."
                                       : "Couldn't reach the PS5";
  $("#os-pass").value = "";
};

/* Tabs. */
function show(tab) {
  $("#t-send").setAttribute("aria-pressed", tab === "send");
  $("#t-remote").setAttribute("aria-pressed", tab === "remote");
  $("#p-send").hidden = tab !== "send";
  $("#p-remote").hidden = tab !== "remote";
  try { localStorage.setItem("vlcps5-tab", tab); } catch (e) {}
}
$("#t-send").onclick = () => show("send");
$("#t-remote").onclick = () => show("remote");
try { if (localStorage.getItem("vlcps5-tab") === "remote") show("remote"); } catch (e) {}

/* Places and the files in the chosen one. */
async function loadPlaces() {
  const places = await (await fetch("/api/places")).json();
  $("#place").innerHTML = places.map((p, i) => `<option value="${i}">${esc(p)}</option>`).join("");
  loadFiles();
}
async function loadFiles() {
  const list = await (await fetch("/api/files?dir=" + $("#place").value)).json();
  $("#files").innerHTML = list.length ? list.map(f =>
    `<div class="item"><div class="name">${esc(f.name)}</div><div class="meta">${fmtSize(f.size)}</div>
     <button class="del" data-name="${esc(f.name)}">Delete</button></div>`).join("")
    : `<div class="empty">No files here yet</div>`;
}
$("#place").onchange = loadFiles;
$("#files").onclick = async e => {
  const b = e.target.closest(".del");
  if (!b || !confirm("Delete " + b.dataset.name + " from the PS5?")) return;
  await fetch("/api/file?dir=" + $("#place").value + "&name=" + encodeURIComponent(b.dataset.name), { method: "DELETE" });
  loadFiles();
};

/* Uploads, one after another, each with its bar. */
const queue = [];
let busy = false;
function addFiles(files) {
  for (const f of files) {
    const el = document.createElement("div");
    el.className = "item";
    el.innerHTML = `<div class="name">${esc(f.name)}</div><div class="meta">Waiting · ${fmtSize(f.size)}</div><div class="bar"><i></i></div>`;
    $("#queue").prepend(el);
    queue.push({ f, el, dir: $("#place").value });
  }
  next();
}
function next() {
  if (busy || !queue.length) return;
  busy = true;
  const { f, el, dir } = queue.shift();
  const xhr = new XMLHttpRequest(), t0 = Date.now();
  xhr.open("PUT", "/api/upload?dir=" + dir + "&name=" + encodeURIComponent(f.name));
  xhr.upload.onprogress = e => {
    const p = e.loaded / f.size, mbs = e.loaded / 1e6 / Math.max(0.5, (Date.now() - t0) / 1000);
    el.querySelector(".bar i").style.width = (p * 100).toFixed(1) + "%";
    el.querySelector(".meta").textContent = `${Math.round(p * 100)}% · ${fmtSize(e.loaded)} of ${fmtSize(f.size)} · ${mbs.toFixed(1)} MB/s`;
  };
  xhr.onloadend = () => {
    const ok = xhr.status === 200;
    el.classList.add(ok ? "done" : "fail");
    el.querySelector(".bar i").style.width = "100%";
    el.querySelector(".meta").textContent = ok ? "On the PS5 · " + fmtSize(f.size) : "Failed: " + (xhr.responseText || "connection lost");
    busy = false;
    loadFiles();
    next();
  };
  xhr.send(f);
}
$("#pick").onchange = e => { addFiles(e.target.files); e.target.value = ""; };
const drop = $("#drop");
drop.ondragover = e => { e.preventDefault(); drop.classList.add("over"); };
drop.ondragleave = () => drop.classList.remove("over");
drop.ondrop = e => { e.preventDefault(); drop.classList.remove("over"); addFiles(e.dataTransfer.files); };

/* Remote. */
let seeking = false;
async function poll() {
  try {
    const s = await (await fetch("/api/status")).json();
    $("#title").textContent = s.playing ? s.title : "Nothing playing";
    $("#state").textContent = s.playing ? (s.paused ? "Paused" : "Playing") : "Start a video on the console";
    if (!seeking) $("#seek").value = s.length ? Math.round(s.time / s.length * 1000) : 0;
    $("#seek").dataset.len = s.length;
    $("#t-now").textContent = fmtTime(s.time);
    $("#t-len").textContent = fmtTime(s.length);
    $("#vol").textContent = "Volume " + s.volume + "%";
    $("#pp").innerHTML = s.playing && !s.paused ? '<path d="M7 5h4v14H7zm6 0h4v14h-4z"/>' : '<path d="M8 5v14l11-7z"/>';
  } catch (e) {
    $("#state").textContent = "Can't reach the PS5: is VLC still open?";
  }
}
document.querySelectorAll("[data-c]").forEach(b => b.onclick = () =>
  fetch("/api/cmd?c=" + b.dataset.c + "&v=" + (b.dataset.v || 0), { method: "POST" }).then(poll));
$("#seek").oninput = () => { seeking = true; };
$("#seek").onchange = () => {
  const ms = Math.round($("#seek").value / 1000 * (+$("#seek").dataset.len || 0));
  fetch("/api/cmd?c=seek&v=" + ms, { method: "POST" }).then(() => { seeking = false; poll(); });
};
setInterval(() => { if (!$("#p-remote").hidden) poll(); }, 1000);

loadPlaces();
poll();
</script>
</body>
</html>
)VLCPAGE";
