/*
 * The page VLC-PS5 serves to phones and computers (see web.cc): send files to
 * the console and set up subtitle downloads. One file, no outside resources
 * (the console may have no internet). It looks like the app's Modern look:
 * black, VLC orange, a glowing tab bar whose highlight glides, panels that
 * slide in from the side you went to, rows with the orange focus bar.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

static const char WEB_PAGE[] = R"VLCPAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#000000">
<title>VLC for PS5</title>
<style>
:root {
  --bg: #000; --bar: #0f0f13; --card: #121217; --raised: #1a1a21; --hi: #24242d; --line: #22222a;
  --fg: #f5f5f7; --dim: #a3a3b0; --faint: #6c6c79;
  --or: #ff8800; --or2: #ff5e14; --glow: rgba(255, 136, 0, .26); --soft: rgba(255, 136, 0, .1);
  --ok: #34d399; --bad: #f87171;
  --ease: cubic-bezier(.22, .8, .24, 1); --spring: cubic-bezier(.34, 1.4, .5, 1);
  --sans: system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", sans-serif;
}
* { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
html { background: var(--bg); }
body { margin: 0; min-height: 100vh; color: var(--fg); font: 15px/1.45 var(--sans); background: var(--bg);
  overflow-x: hidden; padding-bottom: calc(24px + env(safe-area-inset-bottom, 0)); }
button, input { font: inherit; color: inherit; }
button { cursor: pointer; }
:focus-visible { outline: 2px solid var(--or); outline-offset: 2px; }

/* a slow orange glow behind everything, like the app's backdrop */
.aura { position: fixed; inset: -20vh -20vw auto; height: 70vh; pointer-events: none; z-index: 0;
  background: radial-gradient(60% 60% at 50% 0%, rgba(255, 120, 0, .07), transparent 70%);
  animation: breathe 9s ease-in-out infinite; }
@keyframes breathe { 50% { opacity: .55; transform: scale(1.06); } }

/* title row: the cone and VLC, as in the app */
header { position: relative; z-index: 2; max-width: 980px; margin: 0 auto;
  padding: calc(20px + env(safe-area-inset-top, 0)) 16px 0; display: flex; align-items: center; gap: 12px; }
.cone { width: 40px; height: 40px; flex: none; animation: drop .7s var(--spring) both; }
@keyframes drop { from { transform: translateY(-16px) scale(.8); opacity: 0; } }
header h1 { margin: 0; font-size: 24px; font-weight: 750; letter-spacing: -.02em; line-height: 1.1; }
header h1 small { display: block; font-size: 12.5px; font-weight: 500; color: var(--faint); letter-spacing: 0; }
.link { margin-left: auto; display: inline-flex; align-items: center; gap: 8px; font-size: 13px; color: var(--dim);
  padding: 7px 12px; border-radius: 99px; background: var(--bar); border: 1px solid var(--line); }
.link i { width: 8px; height: 8px; border-radius: 50%; background: var(--ok); position: relative; }
.link i::after { content: ""; position: absolute; inset: -4px; border-radius: 50%; border: 2px solid var(--ok);
  opacity: 0; animation: ping 2.4s ease-out infinite; }
@keyframes ping { 0% { transform: scale(.4); opacity: .7; } 80%, 100% { transform: scale(1.4); opacity: 0; } }
.link.off i, .link.off i::after { background: var(--bad); border-color: var(--bad); }

/* the tab bar: a dark box, the highlight glides under the chosen tab */
nav { position: sticky; top: 0; z-index: 3; padding: 14px 16px 12px; max-width: 980px; margin: 0 auto;
  backdrop-filter: blur(18px); -webkit-backdrop-filter: blur(18px); }
.tabs { position: relative; display: grid; grid-template-columns: repeat(3, 1fr); background: var(--bar);
  border: 1px solid var(--line); border-radius: 18px; padding: 6px; max-width: 620px; margin: 0 auto;
  box-shadow: 0 10px 40px rgba(0, 0, 0, .6); }
.tabs .hl { position: absolute; top: 6px; bottom: 6px; left: 6px; width: 0; border-radius: 13px;
  background: linear-gradient(180deg, #ff9a1f, var(--or)); box-shadow: 0 0 14px var(--glow), 0 0 0 1px rgba(255, 170, 60, .3) inset;
  transition: left .45s var(--spring), width .45s var(--spring); }
.tabs button { position: relative; z-index: 1; border: 0; background: none; color: var(--dim); font-weight: 650;
  font-size: 15px; padding: 11px 8px; border-radius: 13px; display: flex; align-items: center; justify-content: center;
  gap: 8px; transition: color .3s; white-space: nowrap; }
.tabs button svg { width: 18px; height: 18px; flex: none; transition: transform .4s var(--spring); }
.tabs button:hover { color: var(--fg); }
.tabs button[aria-selected="true"] { color: #fff; }
.tabs button[aria-selected="true"] svg { transform: scale(1.12); }
.tabs .n { min-width: 20px; height: 20px; padding: 0 6px; border-radius: 10px; font-size: 11.5px; font-weight: 750;
  display: inline-grid; place-items: center; background: var(--hi); color: var(--dim); transition: background .3s, color .3s; }
.tabs button[aria-selected="true"] .n { background: rgba(0, 0, 0, .25); color: #fff; }
.tabs .n:empty { display: none; }
@media (max-width: 480px) { .tabs button .t { display: none; } .tabs button { padding: 12px 6px; } }

/* panels slide in from the side you moved to */
main { position: relative; z-index: 1; max-width: 980px; margin: 0 auto; padding: 8px 16px 0; }
.panel { display: none; }
.panel.on { display: grid; gap: 16px; animation: var(--in, in-r) .5s var(--ease) both; }
@keyframes in-r { from { opacity: 0; transform: translateX(36px); } }
@keyframes in-l { from { opacity: 0; transform: translateX(-36px); } }
.panel > * { animation: rise .55s var(--ease) both; }
.panel > :nth-child(2) { animation-delay: .06s; } .panel > :nth-child(3) { animation-delay: .12s; }
.panel > :nth-child(4) { animation-delay: .18s; }
@keyframes rise { from { opacity: 0; transform: translateY(14px); } }
h2 { margin: 6px 4px 0; font-size: 26px; font-weight: 750; letter-spacing: -.02em; }
.lead { margin: -8px 4px 0; color: var(--dim); }
.card { background: var(--card); border: 1px solid var(--line); border-radius: 20px; padding: 18px; }
@media (min-width: 760px) { .card { padding: 24px; } h2 { font-size: 30px; } }

/* where files go: chips, the chosen one glows */
.places { display: flex; flex-wrap: wrap; gap: 8px; min-width: 0; }
.places button { border: 1px solid var(--line); background: var(--raised); color: var(--dim); border-radius: 12px;
  padding: 9px 14px; font-size: 14px; font-weight: 600; display: inline-flex; align-items: center; gap: 8px;
  transition: all .3s var(--ease); max-width: 100%; }
.places button svg { width: 16px; height: 16px; flex: none; }
.places button span { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.places button:hover { color: var(--fg); background: var(--hi); }
.places button[aria-pressed="true"] { color: var(--fg); background: var(--soft); border-color: var(--or);
  box-shadow: 0 0 10px rgba(255, 136, 0, .12); }

/* the drop zone */
.drop { position: relative; display: grid; place-items: center; text-align: center; gap: 4px; padding: 44px 16px;
  border-radius: 18px; cursor: pointer; background: var(--raised); overflow: hidden;
  transition: transform .35s var(--spring), background .3s; }
.drop::before { content: ""; position: absolute; inset: 0; border-radius: inherit; padding: 2px;
  background: repeating-linear-gradient(90deg, var(--hi) 0 14px, transparent 14px 24px);
  -webkit-mask: linear-gradient(#000 0 0) content-box, linear-gradient(#000 0 0);
  -webkit-mask-composite: xor; mask-composite: exclude; }
.drop:hover { background: var(--hi); }
.drop.over { transform: scale(1.02); background: var(--soft); }
.drop.over::before { background: repeating-linear-gradient(90deg, var(--or) 0 14px, transparent 14px 24px);
  background-size: 24px 100%; animation: march .6s linear infinite; }
@keyframes march { to { background-position: 24px 0; } }
.drop .ic { width: 64px; height: 64px; border-radius: 20px; display: grid; place-items: center; margin-bottom: 10px;
  color: #fff; background: linear-gradient(180deg, #ff9a1f, var(--or2)); box-shadow: 0 6px 16px var(--glow);
  animation: float 3.2s ease-in-out infinite; }
.drop .ic svg { width: 28px; height: 28px; }
@keyframes float { 50% { transform: translateY(-6px); } }
.drop b { font-size: 18px; font-weight: 700; }
.drop span { color: var(--dim); font-size: 14px; }
.drop input { display: none; }

/* rows: the app's list, a lighter row with the orange bar on the left */
.rows { display: grid; gap: 6px; }
.rows:empty { display: none; }
.row { position: relative; display: grid; grid-template-columns: 44px minmax(0, 1fr) auto; align-items: center;
  gap: 14px; padding: 10px 12px 10px 14px; border-radius: 14px;
  transition: background .25s, box-shadow .25s; }
.row::before { content: ""; position: absolute; left: 0; top: 14px; bottom: 14px; width: 4px; border-radius: 2px;
  background: var(--or); transform: scaleY(0); transition: transform .3s var(--spring); }
.row:hover { background: var(--raised); box-shadow: 0 0 0 1px rgba(255, 136, 0, .14); }
.row:hover::before { transform: scaleY(1); }
.row.new { animation: pop .45s var(--spring) both; }
@keyframes pop { from { opacity: 0; transform: translateY(10px) scale(.97); } }
.row.gone { animation: out .35s var(--ease) both; }
@keyframes out { to { opacity: 0; transform: translateX(30px); } }
.row .ic { width: 44px; height: 44px; border-radius: 12px; background: var(--hi); display: grid; place-items: center;
  color: var(--dim); transition: color .25s, background .25s; }
.row .ic svg { width: 22px; height: 22px; }
.row:hover .ic { color: var(--or); background: var(--soft); }
.row .name { font-weight: 600; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.row .meta { font-size: 13px; color: var(--dim); font-variant-numeric: tabular-nums; display: flex; gap: 8px; align-items: center; }
.tag { font-size: 10.5px; font-weight: 750; letter-spacing: .04em; padding: 2px 7px; border-radius: 6px;
  background: var(--hi); color: var(--dim); text-transform: uppercase; }
.bar { height: 5px; margin-top: 8px; background: var(--hi); border-radius: 3px; overflow: hidden; }
.bar i { position: relative; display: block; height: 100%; width: 0; border-radius: 3px; overflow: hidden;
  background: linear-gradient(90deg, var(--or2), var(--or)); box-shadow: 0 0 6px var(--glow); transition: width .3s linear; }
.bar i::after { content: ""; position: absolute; inset: 0; background: linear-gradient(90deg, transparent, rgba(255, 255, 255, .45), transparent);
  transform: translateX(-100%); animation: shine 1.3s ease-in-out infinite; }
@keyframes shine { to { transform: translateX(100%); } }
.row.done .bar i { background: var(--ok); box-shadow: 0 0 12px rgba(52, 211, 153, .5); }
.row.done .bar i::after, .row.fail .bar i::after { display: none; }
.row.fail .bar i { background: var(--bad); box-shadow: none; }
.row.done .meta { color: var(--ok); } .row.fail .meta { color: var(--bad); }
.act { border: 0; background: none; color: var(--faint); border-radius: 10px; padding: 8px 12px; font-size: 13.5px;
  font-weight: 600; transition: all .25s var(--ease); }
.act:hover { color: var(--fg); background: var(--hi); }
.act.sure { color: #fff; background: var(--bad); }
.acts { display: flex; align-items: center; gap: 2px; }
.act.dl { display: inline-flex; align-items: center; gap: 6px; text-decoration: none; }
.act.dl svg { width: 16px; height: 16px; }
.act.dl:hover { color: var(--or); background: var(--soft); }
@media (max-width: 480px) { .act.dl span { display: none; } .act.dl { padding: 8px 10px; } }
.round { width: 38px; height: 38px; padding: 0; display: grid; place-items: center; border-radius: 12px; flex: none; }
.round svg { width: 18px; height: 18px; }
.spin svg { animation: spin .7s var(--ease); }
@keyframes spin { to { transform: rotate(360deg); } }
.empty { display: grid; place-items: center; gap: 10px; padding: 36px 8px; color: var(--faint); text-align: center; }
.empty svg { width: 54px; height: 54px; opacity: .5; }
.headrow { display: flex; align-items: flex-start; justify-content: space-between; gap: 12px; margin-bottom: 14px; }

/* subtitles */
.status { display: flex; align-items: center; gap: 14px; }
.status .ic { width: 52px; height: 52px; border-radius: 16px; flex: none; display: grid; place-items: center;
  background: var(--hi); color: var(--faint); transition: all .4s var(--ease); }
.status .ic svg { width: 26px; height: 26px; }
.status.ok .ic { background: rgba(52, 211, 153, .14); color: var(--ok); }
.status.bad .ic { background: rgba(248, 113, 113, .14); color: var(--bad); }
.status b { display: block; font-size: 16px; } .status span { color: var(--dim); font-size: 13.5px; }
.form { display: grid; gap: 14px; margin-top: 20px; }
.form .two { display: grid; gap: 14px; }
@media (min-width: 760px) { .form .two { grid-template-columns: 1fr 1fr; } }
.field { position: relative; }
.field input { width: 100%; padding: 22px 14px 8px; background: var(--raised); border: 1px solid var(--line);
  border-radius: 14px; transition: border-color .25s, background .25s, box-shadow .25s; }
.field label { position: absolute; left: 15px; top: 15px; color: var(--faint); pointer-events: none;
  transition: all .25s var(--ease); }
.field input:focus { outline: none; border-color: var(--or); background: #0b0b0e; box-shadow: 0 0 0 4px var(--soft); }
.field input:focus + label, .field input:not(:placeholder-shown) + label { top: 6px; font-size: 11.5px; color: var(--or); }
.field label em { font-style: normal; color: var(--faint); margin-left: 6px; }
.go { position: relative; overflow: hidden; border: 0; border-radius: 14px; padding: 15px 18px; font-weight: 750; font-size: 16px;
  color: #fff; background: linear-gradient(180deg, #ff9a1f, var(--or)); box-shadow: 0 4px 14px var(--glow);
  transition: transform .2s var(--spring), box-shadow .3s, opacity .3s; }
.go:hover { transform: translateY(-2px); box-shadow: 0 6px 18px var(--glow); }
.go:active { transform: scale(.98); }
.go:disabled { opacity: .55; cursor: default; transform: none; }
.go.busy::after { content: ""; position: absolute; inset: 0; background: linear-gradient(90deg, transparent, rgba(255, 255, 255, .35), transparent);
  animation: shine 1s ease-in-out infinite; transform: translateX(-100%); }
.help { color: var(--faint); font-size: 13.5px; margin: 0; }
.steps { display: grid; gap: 12px; counter-reset: s; margin: 0; padding: 0; list-style: none; }
.steps li { display: flex; gap: 12px; align-items: flex-start; color: var(--dim); font-size: 14px; }
.steps li::before { counter-increment: s; content: counter(s); flex: none; width: 24px; height: 24px; border-radius: 8px;
  display: grid; place-items: center; font-size: 12px; font-weight: 750; background: var(--soft); color: var(--or); }
.steps a { color: var(--or); font-weight: 600; text-decoration: none; }
.steps a:hover { text-decoration: underline; }

/* toasts, as the app shows them */
.toasts { position: fixed; z-index: 9; left: 50%; bottom: calc(20px + env(safe-area-inset-bottom, 0)); transform: translateX(-50%);
  display: grid; gap: 8px; justify-items: center; width: min(92vw, 420px); pointer-events: none; }
.toast { display: flex; align-items: center; gap: 10px; padding: 12px 18px; border-radius: 16px; background: rgba(28, 28, 36, .92);
  backdrop-filter: blur(12px); -webkit-backdrop-filter: blur(12px); border: 1px solid var(--line); font-weight: 600; font-size: 14.5px;
  box-shadow: 0 12px 40px rgba(0, 0, 0, .6); animation: tin .45s var(--spring) both; max-width: 100%; }
.toast span { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.toast i { width: 9px; height: 9px; border-radius: 50%; background: var(--or); box-shadow: 0 0 10px var(--glow); flex: none; }
.toast.ok i { background: var(--ok); box-shadow: 0 0 10px rgba(52, 211, 153, .6); }
.toast.bad i { background: var(--bad); box-shadow: 0 0 10px rgba(248, 113, 113, .6); }
.toast.bye { animation: tout .35s var(--ease) both; }
@keyframes tin { from { opacity: 0; transform: translateY(20px) scale(.9); } }
@keyframes tout { to { opacity: 0; transform: translateY(10px) scale(.95); } }
footer { position: relative; z-index: 1; text-align: center; color: var(--faint); font-size: 12.5px; padding: 28px 16px 0; }
@media (prefers-reduced-motion: reduce) { *, *::before, *::after { animation: none !important; transition: none !important; } }
</style>
</head>
<body>
<div class="aura"></div>
<header>
  <svg class="cone" viewBox="0 0 64 64" aria-hidden="true"><path d="M32 4 9 56h46z" fill="#ff8800"/><path d="M24 22h16l2.6 6H21.4zM18.6 34h26.8l2.6 6H16z" fill="#fff"/><rect x="5" y="54" width="54" height="7" rx="2.5" fill="#ff5e14"/></svg>
  <h1>VLC<small>for PS5</small></h1>
  <span class="link" id="link"><i></i><span id="link-t">Connected</span></span>
</header>

<nav><div class="tabs" role="tablist">
  <div class="hl" id="hl"></div>
  <button role="tab" data-tab="send"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 15V4M7 9l5-5 5 5M5 20h14"/></svg><span class="t">Send</span><span class="n" id="n-send"></span></button>
  <button role="tab" data-tab="files"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/></svg><span class="t">On the PS5</span><span class="n" id="n-files"></span></button>
  <button role="tab" data-tab="subs"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="5" width="18" height="14" rx="3"/><path d="M7 13h4M13 13h4M7 16h7"/></svg><span class="t">Subtitles</span></button>
</div></nav>

<main>
  <section class="panel" id="p-send">
    <h2>Send files</h2>
    <p class="lead">Videos, music, photos, subtitles, anything. They show up in VLC right away.</p>
    <div class="card" style="display:grid;gap:16px">
      <div class="places" data-places></div>
      <label class="drop" id="drop">
        <input id="pick" type="file" multiple>
        <div class="ic"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M12 15V4M7 9l5-5 5 5M5 20h14"/></svg></div>
        <b>Choose files</b>
        <span id="drop-t">or drop them anywhere on this page</span>
      </label>
      <div class="rows" id="queue"></div>
    </div>
  </section>

  <section class="panel" id="p-files">
    <h2>On the PS5</h2>
    <p class="lead">What's in that folder: download a file to this device, or delete it from the PS5.</p>
    <div class="card">
      <div class="headrow"><div class="places" data-places></div>
        <button class="act round" id="refresh" aria-label="Refresh"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 11a8 8 0 1 0-2.3 5.7M20 4v7h-7"/></svg></button></div>
      <div class="rows" id="files"></div>
    </div>
  </section>

  <section class="panel" id="p-subs">
    <h2>Subtitle downloads</h2>
    <p class="lead">VLC finds subtitles on OpenSubtitles with your own free API key.</p>
    <div class="card">
      <div class="status" id="os-status"><div class="ic"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><circle cx="8" cy="15" r="4"/><path d="m11 12 9-9M17 6l3 3M14 9l2 2"/></svg></div>
        <div><b id="os-title">No key yet</b><span id="os-sub">Paste one below to turn subtitle downloads on.</span></div></div>
      <form class="form" id="os-form" autocomplete="off">
        <div class="field"><input id="os-key" placeholder=" " autocapitalize="off" spellcheck="false"><label for="os-key">API key</label></div>
        <div class="two">
          <div class="field"><input id="os-user" placeholder=" " autocomplete="username" autocapitalize="off" spellcheck="false"><label for="os-user">User name<em>optional</em></label></div>
          <div class="field"><input id="os-pass" type="password" placeholder=" " autocomplete="current-password"><label for="os-pass">Password<em>optional</em></label></div>
        </div>
        <button class="go" id="os-save" type="submit">Save on the PS5</button>
      </form>
    </div>
    <div class="card" style="display:grid;gap:14px">
      <ol class="steps">
        <li><span>Sign in on <a href="https://www.opensubtitles.com/consumers" target="_blank" rel="noopener">opensubtitles.com/consumers</a> and create an API consumer.</span></li>
        <li><span>Copy its API key and paste it above.</span></li>
        <li><span>An account is optional: it gives more downloads a day.</span></li>
      </ol>
      <p class="help">The key stays on your PS5. This page never shows it again.</p>
    </div>
  </section>
</main>
<footer>Works on the same network as the PS5. Nothing leaves your home.</footer>
<div class="toasts" id="toasts"></div>

<script>
const $ = s => document.querySelector(s), $$ = s => [...document.querySelectorAll(s)];
const esc = t => String(t).replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const size = b => b >= 1e9 ? (b / 1e9).toFixed(2) + " GB" : b >= 1e6 ? (b / 1e6).toFixed(1) + " MB"
                : Math.max(1, Math.round(b / 1e3)) + " KB";
const ext = n => { const i = n.lastIndexOf("."); return i > 0 ? n.slice(i + 1).toLowerCase() : ""; };
const save = (k, v) => { try { localStorage.setItem(k, v); } catch (e) {} };
const load = k => { try { return localStorage.getItem(k); } catch (e) { return null; } };

/* Icons by kind of file. */
const P = 'fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"';
const ICON = {
  video: `<svg viewBox="0 0 24 24" ${P}><rect x="3" y="5" width="18" height="14" rx="3"/><path d="m10 9 5 3-5 3z"/></svg>`,
  audio: `<svg viewBox="0 0 24 24" ${P}><path d="M9 18V5l11-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="17" cy="16" r="3"/></svg>`,
  image: `<svg viewBox="0 0 24 24" ${P}><rect x="3" y="4" width="18" height="16" rx="3"/><circle cx="9" cy="10" r="2"/><path d="m21 16-5-5-9 9"/></svg>`,
  sub: `<svg viewBox="0 0 24 24" ${P}><rect x="3" y="5" width="18" height="14" rx="3"/><path d="M7 13h4M13 13h4M7 16h7"/></svg>`,
  zip: `<svg viewBox="0 0 24 24" ${P}><path d="M6 3h9l4 4v14H6z"/><path d="M11 3v2M11 7v2M11 11v2M10 15h2v3h-2z"/></svg>`,
  file: `<svg viewBox="0 0 24 24" ${P}><path d="M6 3h9l4 4v14H6z"/><path d="M14 3v5h5"/></svg>`,
  folder: `<svg viewBox="0 0 24 24" ${P}><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/></svg>`,
  drive: `<svg viewBox="0 0 24 24" ${P}><rect x="3" y="7" width="18" height="10" rx="3"/><path d="M17 12h.01M7 12h6"/></svg>`,
};
const KIND = [["video", "mp4 mkv avi mov ts m2ts mts webm wmv flv mpg mpeg m4v 3gp ogv vob iso"],
  ["audio", "mp3 flac m4a aac ogg opus wav wma alac ape aiff m3u m3u8 pls"],
  ["image", "jpg jpeg png gif bmp webp tga"], ["sub", "srt ass ssa vtt sub idx sup"], ["zip", "zip rar 7z"]];
const kind = n => { const x = ext(n); for (const [k, l] of KIND) if (l.split(" ").includes(x)) return k; return "file"; };

/* Toasts. */
function toast(text, cls = "") {
  const t = document.createElement("div");
  t.className = "toast " + cls;
  t.innerHTML = `<i></i><span>${esc(text)}</span>`;
  $("#toasts").append(t);
  while ($("#toasts").children.length > 3) $("#toasts").firstChild.remove();
  setTimeout(() => { t.classList.add("bye"); setTimeout(() => t.remove(), 350); }, 3200);
}

/* Connection. */
let wasOnline = true;
function online(ok) {
  $("#link").classList.toggle("off", !ok);
  $("#link-t").textContent = ok ? "Connected" : "Can't reach the PS5";
  if (ok && !wasOnline) toast("Connected to the PS5 again", "ok");
  wasOnline = ok;
}
async function api(url, opts) {
  try { const r = await fetch(url, opts); online(true); return r; }
  catch (e) { online(false); return null; }
}

/* Tabs: the highlight glides to the tab, the panel slides in from that side. */
const TABS = ["send", "files", "subs"];
let tab = null;
function moveHl() {
  const b = $(`.tabs [data-tab="${tab}"]`), hl = $("#hl");
  if (!b) return;
  hl.style.left = b.offsetLeft + "px";
  hl.style.width = b.offsetWidth + "px";
}
function show(name, first) {
  if (name === tab) return;
  const from = TABS.indexOf(tab), to = TABS.indexOf(name);
  tab = name;
  save("vlcps5-tab", name);
  $$(".tabs button").forEach(b => b.setAttribute("aria-selected", b.dataset.tab === name));
  $$(".panel").forEach(p => p.classList.remove("on"));
  const p = $("#p-" + name);
  p.style.setProperty("--in", first ? "rise" : to < from ? "in-l" : "in-r");
  void p.offsetWidth;
  p.classList.add("on");
  /* the first time the highlight just sits there; after, it glides */
  const hl = $("#hl");
  if (first) hl.style.transition = "none";
  moveHl();
  if (first) requestAnimationFrame(() => requestAnimationFrame(() => hl.style.transition = ""));
  if (name === "files") loadFiles();
}
$$(".tabs button").forEach(b => b.onclick = () => show(b.dataset.tab));
addEventListener("resize", moveHl);
/* swipe across the page (a phone) to change tab */
let sx = null, sy = 0;
addEventListener("touchstart", e => { if (e.target.closest("input")) return; sx = e.touches[0].clientX; sy = e.touches[0].clientY; }, { passive: true });
addEventListener("touchend", e => {
  if (sx === null) return;
  const dx = e.changedTouches[0].clientX - sx, dy = e.changedTouches[0].clientY - sy;
  sx = null;
  if (Math.abs(dx) > 70 && Math.abs(dx) > Math.abs(dy) * 1.6) {
    const i = TABS.indexOf(tab) + (dx < 0 ? 1 : -1);
    if (i >= 0 && i < TABS.length) show(TABS[i]);
  }
});

/* Where files go: the same choice on both tabs. */
let places = [], place = +load("vlcps5-place") || 0;
function drawPlaces() {
  $$("[data-places]").forEach(box => {
    box.innerHTML = places.map((p, i) => `<button type="button" data-i="${i}" aria-pressed="${i === place}">` +
      `${/usb|ext|drive/i.test(p) ? ICON.drive : ICON.folder}<span>${esc(p)}</span></button>`).join("");
  });
}
async function loadPlaces() {
  const r = await api("/api/places");
  if (!r) return setTimeout(loadPlaces, 3000);
  places = await r.json();
  if (place >= places.length) place = 0;
  drawPlaces();
  loadFiles();
}
document.addEventListener("click", e => {
  const b = e.target.closest("[data-places] button");
  if (!b) return;
  place = +b.dataset.i;
  save("vlcps5-place", place);
  drawPlaces();
  loadFiles(true);
});

/* Files in that place. Only rows that weren't there before animate. */
let shown = null;
async function loadFiles(fresh) {
  const r = await api("/api/files?dir=" + place);
  if (!r) return;
  const list = await r.json();
  if (fresh) shown = null;
  $("#n-files").textContent = list.length || "";
  $("#files").innerHTML = list.length ? list.map(f => {
    const k = kind(f.name), isNew = !shown || !shown.has(f.name);
    return `<div class="row${isNew ? " new" : ""}"><div class="ic">${ICON[k]}</div>
      <div style="min-width:0"><div class="name" title="${esc(f.name)}">${esc(f.name)}</div>
      <div class="meta">${ext(f.name) ? `<span class="tag">${esc(ext(f.name).slice(0, 4))}</span>` : ""}${size(f.size)}</div></div>
      <div class="acts"><a class="act dl" href="/api/download?dir=${place}&name=${encodeURIComponent(f.name)}" download="${esc(f.name)}" data-name="${esc(f.name)}" title="Download"><svg viewBox="0 0 24 24" ${P}><path d="M12 4v11M7 10l5 5 5-5M5 20h14"/></svg><span>Download</span></a>
      <button class="act del" data-name="${esc(f.name)}">Delete</button></div></div>`;
  }).join("") : `<div class="empty">${ICON.folder}<span>No files here yet</span></div>`;
  shown = new Set(list.map(f => f.name));
}
$("#refresh").onclick = () => {
  const b = $("#refresh");
  b.classList.remove("spin"); void b.offsetWidth; b.classList.add("spin");
  loadFiles(true);
};
/* Delete: a first tap turns it red, a second one deletes. */
$("#files").onclick = async e => {
  const d = e.target.closest(".dl");
  if (d) { toast("Downloading " + d.dataset.name); return; } /* the browser saves it */
  const b = e.target.closest(".del");
  if (!b) return;
  if (!b.classList.contains("sure")) {
    b.classList.add("sure"); b.textContent = "Delete?";
    setTimeout(() => { b.classList.remove("sure"); b.textContent = "Delete"; }, 2500);
    return;
  }
  const row = b.closest(".row");
  const r = await api("/api/file?dir=" + place + "&name=" + encodeURIComponent(b.dataset.name), { method: "DELETE" });
  if (r && r.ok) {
    row.classList.add("gone");
    toast("Deleted " + b.dataset.name);
    setTimeout(loadFiles, 350);
  } else {
    toast("Couldn't delete it", "bad");
  }
};

/* Uploads, one after another; the Send tab shows how far the queue is. */
const queue = [];
let current = null;
function badge() {
  const left = queue.length + (current ? 1 : 0);
  $("#n-send").textContent = !left ? "" : current && current.pct != null && !queue.length ? current.pct + "%" : left;
}
function addFiles(files) {
  if (!files.length) return;
  for (const f of files) {
    const el = document.createElement("div");
    el.className = "row new";
    el.innerHTML = `<div class="ic">${ICON[kind(f.name)]}</div>
      <div style="min-width:0"><div class="name">${esc(f.name)}</div><div class="meta">Waiting · ${size(f.size)}</div><div class="bar"><i></i></div></div>
      <button class="act round stop" aria-label="Cancel"><svg viewBox="0 0 24 24" ${P}><path d="M6 6l12 12M18 6 6 18"/></svg></button>`;
    const job = { f, el, dir: place };
    el.querySelector(".stop").onclick = () => {
      if (current === job) { job.cancelled = true; job.xhr.abort(); }
      else { queue.splice(queue.indexOf(job), 1); el.classList.add("gone"); setTimeout(() => el.remove(), 350); badge(); }
    };
    $("#queue").append(el);
    queue.push(job);
  }
  toast(files.length === 1 ? "Sending " + files[0].name : "Sending " + files.length + " files");
  if (tab !== "send") show("send");
  next();
}
function next() {
  badge();
  if (current || !queue.length) return;
  const job = current = queue.shift();
  const { f, el } = job, meta = el.querySelector(".meta"), bar = el.querySelector(".bar i");
  const xhr = job.xhr = new XMLHttpRequest(), t0 = Date.now();
  xhr.open("PUT", "/api/upload?dir=" + job.dir + "&name=" + encodeURIComponent(f.name));
  xhr.upload.onprogress = e => {
    const p = f.size ? e.loaded / f.size : 1, mbs = e.loaded / 1e6 / Math.max(.5, (Date.now() - t0) / 1000);
    job.pct = Math.round(p * 100);
    bar.style.width = (p * 100).toFixed(1) + "%";
    meta.textContent = `${job.pct}% · ${size(e.loaded)} of ${size(f.size)} · ${mbs.toFixed(1)} MB/s`;
    badge();
  };
  xhr.onloadend = () => {
    const ok = xhr.status === 200;
    el.classList.add(ok ? "done" : "fail");
    bar.style.width = "100%";
    meta.textContent = ok ? "On the PS5 · " + size(f.size) : job.cancelled ? "Cancelled" : "Failed: " + (xhr.responseText || "connection lost");
    el.querySelector(".stop").remove();
    if (ok) {
      toast(f.name + " is on the PS5", "ok");
      setTimeout(() => { el.classList.add("gone"); setTimeout(() => el.remove(), 350); }, 4000);
    } else if (!job.cancelled) {
      toast("Couldn't send " + f.name, "bad");
    }
    current = null;
    if (job.dir === place) loadFiles();
    next();
  };
  xhr.send(f);
}
$("#pick").onchange = e => { addFiles([...e.target.files]); e.target.value = ""; };
/* Drop anywhere: the zone lights up and its border marches. */
const drop = $("#drop");
let depth = 0;
const dropText = on => { drop.classList.toggle("over", on); $("#drop-t").textContent = on ? "Let go to send" : "or drop them anywhere on this page"; };
addEventListener("dragenter", e => { e.preventDefault(); if (depth++ === 0) { if (tab !== "send") show("send"); dropText(true); } });
addEventListener("dragleave", () => { if (--depth <= 0) { depth = 0; dropText(false); } });
addEventListener("dragover", e => e.preventDefault());
addEventListener("drop", e => { e.preventDefault(); depth = 0; dropText(false); addFiles([...e.dataTransfer.files]); });
addEventListener("beforeunload", e => { if (current) { e.preventDefault(); e.returnValue = ""; } });

/* OpenSubtitles: the PS5 keeps the key and checks it; the card shows how that went. */
let waiting = false;
async function osubStatus() {
  const r = await api("/api/opensubtitles");
  if (!r) return;
  const s = await r.json();
  const st = $("#os-status");
  st.classList.toggle("ok", s.key && !s.failed && !s.checking);
  st.classList.toggle("bad", s.key && s.failed && !s.checking);
  $("#os-title").textContent = !s.key ? "No key yet" : s.checking ? "Checking the key…" : s.failed ? "The key didn't work" : "Subtitle downloads are on";
  $("#os-sub").textContent = !s.key ? "Paste one below to turn subtitle downloads on."
    : s.checking ? "The PS5 is asking OpenSubtitles." : s.message ? s.message + (s.user ? " · " + s.user : "")
    : s.user ? "Signed in as " + s.user : "Using your API key";
  if (waiting && !s.checking && s.message) {
    waiting = false;
    $("#os-save").classList.remove("busy");
    toast(s.message, s.failed ? "bad" : "ok");
  }
}
$("#os-form").onsubmit = async e => {
  e.preventDefault();
  const key = $("#os-key").value.trim();
  if (!key) { toast("Paste your API key first", "bad"); $("#os-key").focus(); return; }
  const btn = $("#os-save");
  btn.disabled = true; btn.classList.add("busy");
  const body = "key=" + encodeURIComponent(key) + "&user=" + encodeURIComponent($("#os-user").value.trim()) +
               "&pass=" + encodeURIComponent($("#os-pass").value);
  const r = await api("/api/opensubtitles", { method: "POST", body,
    headers: { "Content-Type": "application/x-www-form-urlencoded" } });
  btn.disabled = false;
  $("#os-pass").value = "";
  if (r && r.ok) {
    $("#os-key").value = "";
    waiting = true;
    toast("Sent to the PS5, checking…");
    setTimeout(osubStatus, 600);
    setTimeout(() => { if (waiting) { waiting = false; btn.classList.remove("busy"); } }, 20000);
  } else {
    btn.classList.remove("busy");
    toast("Couldn't reach the PS5", "bad");
  }
};

/* first tab: the address's #send / #files / #subs, else the last one used */
const want = location.hash.slice(1) || load("vlcps5-tab");
show(TABS.includes(want) ? want : "send", true);
addEventListener("load", moveHl);
if (document.fonts) document.fonts.ready.then(moveHl);
loadPlaces();
osubStatus();
setInterval(osubStatus, 2500);
setInterval(() => { if (!current && !document.hidden && tab === "files") loadFiles(); }, 15000);
</script>
</body>
</html>
)VLCPAGE";
