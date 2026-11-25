// Rekodr keys for Stream Deck: save the last so long, pause and resume recording, open the window.
//
// No dependencies: Stream Deck's bundled Node 24 has WebSocket built in, and this file speaks Stream Deck's
// plugin protocol directly. It talks to Rekodr over its named pipe (app\pipe.cpp): commands go in as lines
// ("save 30s"), and Rekodr sends its state and the result of each save back, so the keys show what happened.
// While Rekodr isn't running, a key press starts it with the key's command.
import fs from "node:fs";
import net from "node:net";
import path from "node:path";
import { spawn } from "node:child_process";
import { C, card, saveKey, tick, cross, film, track, sliders, dot, bars, ring } from "./render.mjs";

const argv = process.argv.slice(2);
const arg = k => argv[argv.indexOf(k) + 1];
const PORT = arg("-port"), UUID = arg("-pluginUUID"), REGISTER = arg("-registerEvent");
const PREFIX = "dev.calvbot.rekodr.";

// ---------- Stream Deck connection ----------
const sd = new WebSocket(`ws://127.0.0.1:${PORT}`);
const send = o => sd.readyState === 1 && sd.send(JSON.stringify(o));
const log = msg => send({ event: "logMessage", payload: { message: "[rekodr] " + msg } });
sd.onopen = () => { send({ event: REGISTER, uuid: UUID }); rekodr.connect(); };
sd.onclose = () => process.exit(0);
sd.onmessage = m => {
  try { onEvent(JSON.parse(m.data)); } catch (e) { log(e.stack); }
};

// ---------- Rekodr ----------
// The exe to start when Rekodr isn't running: this plugin folder is a link into the Rekodr repo, so the
// build is two folders up. REKODR_EXE overrides it.
function exePath() {
  if (process.env.REKODR_EXE) return process.env.REKODR_EXE;
  let dir = import.meta.dirname;
  try { dir = fs.realpathSync(dir); } catch {}
  return path.resolve(dir, "..", "..", "app", "build", "rekodr.exe");
}

const rekodr = {
  pipe: null, connected: false, state: null, buffer: "", waiting: [], // waiting: Save keys whose save hasn't finished
  connect() {
    clearTimeout(this.retry);
    const pipe = this.pipe = net.connect(String.raw`\\.\pipe\rekodr`);
    pipe.setEncoding("utf8");
    pipe.on("connect", () => { this.connected = true; pipe.write("hello streamdeck\n"); renderAll(); });
    pipe.on("data", d => {
      this.buffer += d;
      let nl;
      while ((nl = this.buffer.indexOf("\n")) >= 0) {
        const line = this.buffer.slice(0, nl);
        this.buffer = this.buffer.slice(nl + 1);
        try { this.onMessage(JSON.parse(line)); } catch (e) { log("bad line from rekodr: " + line); }
      }
    });
    pipe.on("error", () => {}); // not running: "close" follows
    pipe.on("close", () => {
      if (this.pipe !== pipe) return;
      const was = this.connected;
      this.connected = false;
      this.state = null;
      this.buffer = "";
      this.waiting.splice(0).forEach(k => k.finished(false));
      if (was) renderAll();
      this.retry = setTimeout(() => this.connect(), 2000);
    });
  },
  onMessage(m) {
    if (m.type === "state") { this.state = m; renderAll(); }
    else if (m.type === "saved" && m.kind === "clip") {
      if (!m.ok) log("save failed: " + m.error);
      this.waiting.shift()?.finished(m.ok);
    }
  },
  // Sends a command to the running Rekodr, or starts Rekodr with it. Returns false if neither worked.
  command(text) {
    if (this.connected) { this.pipe.write(text + "\n"); return true; }
    const exe = exePath();
    if (!fs.existsSync(exe)) { log("Rekodr isn't running and " + exe + " doesn't exist"); return false; }
    spawn(exe, text.split(" "), { detached: true, stdio: "ignore" }).unref();
    return true;
  },
};

// "23h", "45m", "now": how much footage the buffer holds.
function bufferLength() {
  const oldest = rekodr.state?.oldest_us;
  if (!oldest) return "";
  const mins = Math.floor((Date.now() * 1000 - oldest) / 60e6);
  if (mins < 1) return "";
  return mins < 60 ? mins + "m" : Math.floor(mins / 60) + "h";
}

// ---------- keys ----------
const keys = new Map();
const renderAll = () => keys.forEach(k => k.render());
setInterval(renderAll, 60000); // the buffer length on the Record key

function onEvent(e) {
  const k = keys.get(e.context);
  switch (e.event) {
    case "willAppear": {
      const Kind = KINDS[e.action?.slice(PREFIX.length)];
      if (!Kind) return;
      const key = new Kind(e.context, e.payload.settings || {});
      keys.set(e.context, key);
      key.render();
      break;
    }
    case "willDisappear": if (k) { k.gone = true; keys.delete(e.context); } break;
    case "didReceiveSettings": if (k) { k.settings = { ...k.defaults(), ...e.payload.settings }; k.render(); } break;
    case "keyDown": k?.down(); break;
  }
}

class Key {
  constructor(ctx, settings) {
    this.ctx = ctx;
    this.settings = { ...this.defaults(), ...settings };
    this.lastImage = "";
    // Fill in defaults so the property inspector shows them.
    if (Object.keys(this.defaults()).some(k => !(k in settings)))
      send({ event: "setSettings", context: ctx, payload: this.settings });
  }
  defaults() { return {}; }
  image(svg) {
    if (svg === this.lastImage || this.gone) return;
    this.lastImage = svg;
    send({ event: "setImage", context: this.ctx, payload: { image: "data:image/svg+xml;base64," + Buffer.from(svg).toString("base64"), target: 0 } });
  }
  alert() { send({ event: "showAlert", context: this.ctx }); }
  down() {}
  render() {}
}

// Save: the last `length` of the buffer ("30s", "2m", "1h") as a clip. The key shows SAVING until Rekodr says
// the file is written, then a tick (or a cross) for a moment.
class Save extends Key {
  defaults() { return { length: "30s" }; }
  down() {
    const length = String(this.settings.length).trim() || "30s";
    const running = rekodr.connected;
    if (!rekodr.command("save " + length)) return this.alert();
    if (!running) return; // Rekodr is starting with the command; there's no pipe yet to hear the result on
    clearTimeout(this.timer);
    this.phase = "saving";
    rekodr.waiting.push(this);
    this.timer = setTimeout(() => { // no answer: stop waiting
      rekodr.waiting = rekodr.waiting.filter(k => k !== this);
      this.finished(false);
    }, 10 * 60000);
    this.render();
  }
  finished(ok) {
    clearTimeout(this.timer);
    this.phase = ok ? "saved" : "failed";
    if (!ok) this.alert();
    this.timer = setTimeout(() => { this.phase = ""; this.render(); }, ok ? 2500 : 4000);
    this.render();
  }
  render() {
    const length = String(this.settings.length).trim() || "30s";
    if (this.phase === "saved") return this.image(card({ accent: C.green, glow: 0.4, label: "SAVED", body: tick(C.green) }));
    if (this.phase === "failed") return this.image(card({ accent: C.red, glow: 0.45, label: "FAILED", body: cross(C.red) }));
    if (this.phase === "saving") return this.image(saveKey(length, "SAVING", C.amber, 0.4));
    if (!rekodr.connected) return this.image(saveKey(length, "NOT RUNNING", C.dim, 0.15));
    if (rekodr.state?.paused) return this.image(saveKey(length, "PAUSED", C.amber));
    this.image(saveKey(length, "SAVE CLIP", C.red));
  }
}

// Record: shows whether Rekodr is recording and how much the buffer holds; a press pauses or resumes.
class Record extends Key {
  down() { if (!rekodr.command(rekodr.connected ? "toggle" : "resume")) this.alert(); }
  render() {
    const s = rekodr.state;
    if (!rekodr.connected || !s) return this.image(card({ accent: C.dim, glow: 0.12, label: "NOT RUNNING", body: ring(C.dim) }));
    if (s.paused) return this.image(card({ accent: C.amber, label: "PAUSED", body: bars(C.amber) }));
    if (!s.recording) {
      return s.error
        ? this.image(card({ accent: C.red, glow: 0.45, label: "NO VIDEO", body: cross(C.red) }))
        : this.image(card({ accent: C.grey, glow: 0.15, label: "STARTING", body: ring(C.grey) }));
    }
    // Still recording, so the red dot stays; the frame and label say what needs a look
    if (s.strain) return this.image(card({ accent: C.amber, label: "SLOW PC", body: dot(C.red) }));
    if (s.backup === "failed") return this.image(card({ accent: C.amber, label: "BACKUP FAILED", body: dot(C.red) }));
    const held = bufferLength();
    this.image(card({ accent: C.red, label: held ? "REC  " + held : "REC", body: dot(C.red) }));
  }
}

// Open: the Rekodr window on one of its screens.
const SCREENS = {
  library: { command: "open", label: "CLIPS", icon: film },
  timeline: { command: "timeline", label: "TIMELINE", icon: track },
  options: { command: "options", label: "OPTIONS", icon: sliders },
};
class Open extends Key {
  defaults() { return { screen: "library" }; }
  screen() { return SCREENS[String(this.settings.screen).trim().toLowerCase()] || SCREENS.library; }
  down() { if (!rekodr.command(this.screen().command)) this.alert(); }
  render() {
    const s = this.screen();
    this.image(card({ accent: C.red, label: s.label, body: s.icon(C.red) }));
  }
}

const KINDS = { save: Save, record: Record, open: Open };
