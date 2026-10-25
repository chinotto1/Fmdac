// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
//
// The FM-1 USB-MIDI update protocol over WebMIDI (after tools/fm1_install.py).
// Step 1: the running firmware (official
// or Felucca) reads parts of the package and stages the update loader. Step 2:
// the loader reads the whole image and writes it. Both steps are "the device
// asks, we answer": cmd 0x30 read requests on the logical image.

const HS_QUERY = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7];
const UPGRADE = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7];
const MAXDATA = 512;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const IS_OTA = (id) => /^ota-/i.test(id.model);
// Local changes: explicit FM-1 identities, matching input/output ports, selected
// device checks, checksummed replies, exact loader/product matching, bounded
// requests, cleanup on failure and final identity verification for every update.
export const FM1_PORT = /fm[- ]?1|fmdac|felucca|skiff|sloop|clipper|fzfm|ota|composite|sinco|usb-midi/i;

// errors carry a code the pages translate: notfound, model, stopped, noloader, lost, noreturn,
// mismatch (detail: the identity the device reports), badreq
function fail(code, msg, detail = "") { const e = new Error(msg); e.code = code; e.detail = detail; return e; }

export function pack7(data) {
  const out = []; let acc = 0, nb = 0;
  for (const b of data) {
    acc |= b << nb; nb += 8;
    while (nb >= 7) { out.push(acc & 0x7F); acc >>>= 7; nb -= 7; }
  }
  if (nb) out.push(acc & 0x7F);
  return out;
}

export function unpack7(s) {
  const out = []; let acc = 0, nb = 0;
  for (const b of s) {
    acc |= (b & 0x7F) << nb; nb += 7;
    while (nb >= 8) { out.push(acc & 0xFF); acc >>>= 8; nb -= 8; }
  }
  return out;
}

function response(addr, data, fl = 0) {
  const len = data.length;
  const body = [0x00, 0x59, 0x30, (len + 8) & 0xFF, ((len + 8) >> 8) & 0xFF, 0, fl,
    addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 24) & 0xFF,
    len & 0xFF, (len >> 8) & 0xFF, 0, ...data];
  let s = 0;
  for (let i = 6; i < body.length; i++) s += body[i];
  body.push(~s & 0xFF);
  return [0xF0, ...pack7(body), 0xF7];
}

function parseRequest(pkt) {
  if (pkt[0] !== 0xF0 || pkt[pkt.length - 1] !== 0xF7) return null;
  const u = unpack7(pkt.slice(1, -1));
  if (u.length !== 15 || u[0] !== 0 || u[1] !== 0x59 || u[2] !== 0x30 || u[3] !== 8 || u[4] || u[5]) return null;
  let s = 0;
  for (let i = 6; i < 14; i++) s += u[i];
  if ((~s & 0xFF) !== u[14]) return null;
  return { fl: u[6], addr: (u[7] | (u[8] << 8) | (u[9] << 16) | (u[10] << 24)) >>> 0, len: u[11] | (u[12] << 8) | (u[13] << 16) };
}

function parseIdentity(pkt) {
  if (pkt[0] !== 0xF0 || pkt[pkt.length - 1] !== 0xF7) return null;
  const d = unpack7(pkt.slice(1, -1));
  if (d.length !== 34 || d[0] !== 0 || d[1] !== 0x59 || d[2] !== 0x11 || d[3] !== 27 || d[4] || d[5]) return null;
  if ((~d.slice(6, 33).reduce((a, b) => a + b, 0) & 0xFF) !== d[33]) return null;
  const txt = String.fromCharCode(...d.slice(6, 33)).replace(/\0+$/, "");
  const m = /^((?:ota-)?FM-1)_(\d{3})$/i.exec(txt);
  return m ? { text: txt, model: m[1], version: parseInt(m[2], 10) } : null;
}

// one MIDI in/out pair with a SysEx queue
class Link {
  constructor(input, output) {
    this.input = input; this.output = output; this.q = []; this.waiter = null;
    input.onmidimessage = (ev) => {
      if (ev.data[0] !== 0xF0) return;
      this.q.push(Array.from(ev.data));
      if (this.waiter) { const w = this.waiter; this.waiter = null; w(); }
    };
  }
  get lost() { return this.input.state === "disconnected" || this.output.state === "disconnected"; }
  // false when the port is gone (unplugged): WebMIDI throws on a disconnected output
  send(bytes) {
    try { this.output.send(bytes); return true; } catch (_) { return false; }
  }
  drain() { this.q.length = 0; }
  async read(ms) {
    if (!this.q.length) {
      let tm;
      await Promise.race([new Promise((r) => { this.waiter = r; }), new Promise((r) => { tm = setTimeout(r, ms); })]);
      clearTimeout(tm);
      this.waiter = null;
    }
    return this.q.shift() || null;
  }
  async close() {
    this.input.onmidimessage = null;
    this.q.length = 0;
    // Web MIDI output.send queues USB bytes. Closing the output here can drop
    // the final "success" reply before the loader receives it. As upstream does,
    // keep the ports open across the handover; the browser releases them on exit.
  }
}

async function handshake(link, tries = 3) {
  link.drain();
  for (let t = 0; t < tries; t++) {
    if (!link.send(HS_QUERY)) return null;
    const end = Date.now() + 1000;
    while (Date.now() < end) {
      const p = await link.read(end - Date.now());
      if (!p) break;
      const id = parseIdentity(p);
      if (id) return id;
    }
  }
  return null;
}

export class Updater {
  constructor(access, log, timing = {}) {
    this.access = access;
    this.log = log || (() => {});
    this.timing = {settle:2000, handover:3000, loader:30000, reboot:40000, idle:180000, ...timing};
  }

  // every in/out pair with the same port name that answers the handshake
  async find(filter) {
    const outs = [...this.access.outputs.values()];
    const candidates = [...this.access.inputs.values()].filter(i => i.state !== "disconnected" && FM1_PORT.test(i.name || "") && outs.some(o => o.state !== "disconnected" && o.name === i.name));
    if (candidates.length > 1) throw fail("multiple", "Connect one FM-1 at a time and disconnect other matching MIDI devices.");
    for (const input of this.access.inputs.values()) {
      if (input.state === "disconnected") continue;
      if (!FM1_PORT.test(input.name || "")) continue;
      const matches = outs.filter((o) => o.name === input.name && o.state !== "disconnected");
      if (matches.length > 1) throw fail("multiple", "Connect one FM-1 at a time; multiple matching MIDI outputs were found.");
      if (!matches.length) continue;
      const output = matches[0];
      if (!output) continue;
      try { await input.open(); await output.open(); }
      catch (_) {
        await Promise.allSettled([input, output].map(async port => port.close?.()));
        continue;
      }
      const link = new Link(input, output);
      const id = await handshake(link, 2);
      if (id && (!filter || filter(id))) return { link, id, name: input.name };
      await link.close();
    }
    return null;
  }

  async waitFor(filter, ms) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
      const dev = await this.find(filter);
      if (dev) return dev;
      await sleep(Math.min(1000, this.timing.settle));
    }
    return null;
  }

  // serve read requests until the device asks for one of the finish addresses; stops early
  // when the port disappears (lost: true) instead of waiting out idleMs
  async serve(link, image, finishAddr, idleMs, onProgress) {
    let served = 0, last = Date.now();
    for (;;) {
      if (Date.now() - last > idleMs) return { served, finished: false };
      const pkt = await link.read(Math.min(1000, idleMs));
      if (!pkt) {
        if (link.lost) return { served, finished: false, lost: true };
        if (Date.now() - last > idleMs) return { served, finished: false };
        continue;
      }
      const r = parseRequest(pkt);
      if (!r) continue;
      last = Date.now();
      if (r.addr === 0xE0000000 || r.addr === 0xF0000000) {
        if (r.addr !== finishAddr) throw fail("badreq", "The device completed the wrong update stage.");
        if (!link.send(response(r.addr, [...new TextEncoder().encode("success"), 0])))
          return { served, finished: false, lost: true };
        return { served, finished: true };
      }
      if (!r.len || r.len > MAXDATA || r.addr + r.len > image.length) throw fail("badreq", `bad request ${r.addr.toString(16)}+${r.len}`);
      await sleep(10);
      if (!link.send(response(r.addr, Array.from(image.subarray(r.addr, r.addr + r.len)), r.fl))) {
        return { served, finished: false, lost: true };
      }
      served++;
      if (onProgress) onProgress(served, r.addr, r.len);
    }
  }

  // full install: running firmware -> loader -> new firmware
  async install(image, product, onStep, expectedIdentity = null, expectedPorts = null) {
    if (!/^FM-1_\d{3}$/.test(product || "")) throw fail("model", "A verified target firmware is required.");
    const step = onStep || (() => {});
    const dev = await this.find((id) => !IS_OTA(id));
    if (!dev) throw fail("notfound", "FM-1 not found (USB cable, and no other app using it?)");
    const [model] = product.split("_");
    if (dev.id.model !== model) { await dev.link.close(); throw fail("model", `the device is ${dev.id.text}, the package is for ${product}`); }
    if (expectedIdentity && dev.id.text !== expectedIdentity) { await dev.link.close(); throw fail("changed", "The connected firmware changed. Connect and review it again."); }
    if (expectedPorts && (dev.link.input.id !== expectedPorts[0] || dev.link.output.id !== expectedPorts[1])) { await dev.link.close(); throw fail("changed", "The selected MIDI device changed. Connect and review it again."); }
    step("start", dev.id.text);
    let s1;
    try {
      if (!dev.link.send(UPGRADE)) throw fail("lost", "The MIDI output disconnected before the update started.");
      await sleep(this.timing.settle);
      s1 = await this.serve(dev.link, image, 0xE0000000, 8000, (n) => step("verify", n));
    } finally { await dev.link.close(); }
    if (!s1.finished) throw fail(s1.lost ? "lost" : "stopped", `the device ${s1.lost ? "was disconnected" : "stopped"} after ${s1.served} requests: nothing was written`);
    step("loader");
    await sleep(this.timing.handover);
    const ota = await this.waitFor((id) => id.text === "ota-" + product, this.timing.loader);
    if (!ota) throw fail("noloader", "the update loader did not appear. Replug the USB cable and press Install again: the device stays in update mode until it is finished.");
    const s2 = await this.write(ota, image, step, product);
    if (!s2.finished) throw fail(s2.lost ? "lost" : "stopped", `the loader ${s2.lost ? "was disconnected" : "stopped"} after ${s2.served} requests. Replug and press Install again to resume.`);
    step("reboot");
    await sleep(this.timing.handover);
    const back = await this.waitFor((id) => !IS_OTA(id), this.timing.reboot);
    if (!back) throw fail("noreturn", "the device did not come back: power-cycle it");
    await back.link.close();
    if (back.id.text !== product) throw fail("mismatch", `installed, but the device reports ${back.id.text}`, back.id.text);
    step("done", back.id.text);
    return back.id.text;
  }

  // step 2 with the loader found by find()/waitFor()
  async write(ota, image, step, product) {
    try {
      if (ota.id.text !== "ota-" + product)
        throw fail("foreign", `Finish ${ota.id.text.replace(/^ota-/, "")} with its own matching package before changing firmware.`, ota.id.text);
      if (!ota.link.send(UPGRADE)) throw fail("lost", "The update loader disconnected.");
      await sleep(this.timing.settle);
      return await this.serve(ota.link, image, 0xF0000000, this.timing.idle, (n, addr, len) => step("write", {requests:n, end:addr+len}));
    } finally { await ota.link.close(); }
  }

  // resume: the device is already in update mode (loader) -> true when the write finished
  async resume(image, onStep, product = null, expectedPorts = null) {
    if (!/^FM-1_\d{3}$/.test(product || "")) throw fail("model", "A verified target firmware is required.");
    const model = product.split("_")[0];
    const ota = await this.find(IS_OTA);
    if (!ota) return false;
    if (expectedPorts && (ota.link.input.id !== expectedPorts[0] || ota.link.output.id !== expectedPorts[1])) { await ota.link.close(); throw fail("changed", "The selected MIDI update device changed. Connect and review it again."); }
    const step = onStep || (() => {});
    const result = await this.write(ota, image, step, product);
    if (!result.finished) return false;
    if (product) {
      step("reboot");
      await sleep(this.timing.handover);
      const back = await this.waitFor((id) => id.model === model, this.timing.reboot);
      if (!back) throw fail("noreturn", "The FM-1 did not return after resuming the write.");
      await back.link.close();
      if (back.id.text !== product) throw fail("mismatch", "The FM-1 reports a different firmware after resuming.", back.id.text);
      step("done", back.id.text);
    }
    return true;
  }
}
