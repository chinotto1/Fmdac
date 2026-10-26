// SPDX-License-Identifier: GPL-3.0-only
import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import test from "node:test";
import {FM1_PORT, loaderAccepts, pack7, Updater, WriteMeter} from "./vendor/fm1ota.js";
import {flashSpan, productOf, logicalImage} from "./vendor/fm1pkg.js";
import {
  acceptPackage,
  assertCatalog,
  blockReason,
  inspectPackage,
  loaderMismatch,
  validatePinned,
} from "./packages.js";

const BLOCKS = 20;
const BLK = 0x30;
const KEEP = 0x2F;

function encode(identity, extra = 64) {
  const bytes = new Uint8Array(BLOCKS * BLK + extra);
  for (let i = 0; i < BLOCKS; i++) {
    bytes[i * BLK + KEEP] = i < identity.length ? (identity.charCodeAt(i) + i + 1) & 0xff : 0x7d;
  }
  return bytes;
}

const ready = {
  catalogReady: true,
  supported: true,
  mode: "install",
  release: {enabled: false, filename: "fmdac.fwsc", reason: "unpublished"},
  prepared: null,
  device: null,
};

test("package identity round-trips through the marker bytes", () => {
  const bytes = encode("FM-1_096", 32);
  assert.equal(productOf(bytes), "FM-1_096");
  assert.equal(logicalImage(bytes).length, bytes.length - BLOCKS);
});

test("a short or unmarked file is refused", async () => {
  await assert.rejects(inspectPackage(new Uint8Array(100)), /not an FM-1 package/);
  await assert.rejects(inspectPackage(encode("nope")), /no FM-1 package identity/);
});

test("a pinned release must match identity, size, and checksum", async () => {
  const bytes = encode("FM-1_713", 80);
  const {digest} = await inspectPackage(bytes);
  const release = {
    enabled: true,
    product: "FM-1_713",
    size: bytes.length,
    sha256: digest,
    filename: "skiff-1.3.2.fwsc",
  };
  const checked = await validatePinned(bytes, release);
  assert.equal(checked.product, "FM-1_713");
  const flipped = new Uint8Array(bytes);
  flipped[10] ^= 0xff;
  await assert.rejects(validatePinned(flipped, release), /Checksum mismatch/);
  await assert.rejects(validatePinned(encode("FM-1_900", 80), release), /FM-1_900/);
  await assert.rejects(acceptPackage(encode("FM-1_713", 8), release, true), /file size/);
});

test("an unpinned file keeps the identity it actually carries", async () => {
  const checked = await acceptPackage(encode("FM-1_042"), {enabled: false}, false);
  assert.equal(checked.product, "FM-1_042");
  assert.equal(checked.release, null);
});

test("the install button stays closed until a firmware file and the FM-1 are both ready", () => {
  assert.match(blockReason(ready), /Choose a \.fwsc/);
  assert.match(blockReason({
    ...ready,
    device: {identity: "FM-1_015", name: "FM-1"},
  }), /no Fmdac firmware file/);
  const prepared = {product: "FM-1_096"};
  assert.match(blockReason({...ready, prepared, supported: false}), /Chrome or Edge/);
  assert.match(blockReason({...ready, prepared}), /Connect the FM-1/);
  assert.match(blockReason({
    ...ready,
    prepared,
    device: {identity: "ota-FM-1_713", name: "FM-1"},
  }), /FM-1_713/);
  assert.equal(loaderMismatch("ota-FM-1_096", "FM-1_096"), false);
  assert.equal(loaderMismatch("ota-FM-1_015", "FM-1_016"), false);
  assert.equal(blockReason({
    ...ready,
    mode: "recover",
    release: {enabled: true, filename: "FM-1.fwsc"},
    prepared: {product: "FM-1_016"},
    device: {identity: "ota-FM-1_015", name: "FM-1"},
  }), "");
  assert.equal(blockReason({
    ...ready,
    prepared,
    device: {identity: "FM-1_015", name: "FM-1"},
  }), "");
  assert.match(blockReason({
    ...ready,
    mode: "recover",
    release: {enabled: true, filename: "sloop-2.7.1.fwsc"},
  }), /sloop-2\.7\.1\.fwsc/);
});

test("the published catalog is complete and the page file matches it", async () => {
  const catalog = assertCatalog(JSON.parse(await readFile(new URL("./releases.json", import.meta.url), "utf8")));
  assert.equal(catalog.current.enabled, true);
  assert.equal(catalog.current.product, "FM-1_971");
  assert.equal(catalog.current.filename, "fmdac.fwsc");
  assert.equal(catalog.recovery.length, 3);
  assert.deepEqual(catalog.recovery.map((release) => release.product), ["FM-1_713", "FM-1_900", "FM-1_016"]);
  const bytes = new Uint8Array(await readFile(new URL("./fmdac.fwsc", import.meta.url)));
  const checked = await validatePinned(bytes, catalog.current);
  assert.equal(checked.product, "FM-1_971");
  assert.equal(productOf(bytes), "FM-1_971");
  assert.equal(logicalImage(bytes).length, bytes.length - 20);
});

function jlEnc(src) {
  const out = new Uint8Array(src);
  let key = 0xFFFF;
  for (let i = 0; i < out.length; i++) {
    out[i] ^= key & 0xFF;
    key = ((key << 1) ^ (key & 0x8000 ? 0x1021 : 0)) & 0xFFFF;
  }
  return out;
}

function imageWithFlash(off, size) {
  const img = new Uint8Array(off + size);
  const header = new Uint8Array(0x40);
  header[8] = 1;
  img.set(jlEnc(header), 0);
  const entry = new Uint8Array(0x50);
  entry[8] = off & 255;
  entry[9] = (off >>> 8) & 255;
  entry[10] = (off >>> 16) & 255;
  entry[11] = (off >>> 24) & 255;
  entry[12] = size & 255;
  entry[13] = (size >>> 8) & 255;
  entry[14] = (size >>> 16) & 255;
  entry[15] = (size >>> 24) & 255;
  img.set(jlEnc(entry), 0x40);
  return img;
}

function request(addr, len, lengthByte = 8) {
  const body = [0x00, 0x59, 0x30, lengthByte, 0, 0, 0,
    addr & 255, (addr >>> 8) & 255, (addr >>> 16) & 255, (addr >>> 24) & 255,
    len & 255, (len >>> 8) & 255, (len >>> 16) & 255];
  let sum = 0;
  for (let i = 6; i < body.length; i++) sum += body[i];
  body.push((~sum) & 255);
  return [0xF0, ...pack7(body), 0xF7];
}

function fakeLink(packets) {
  return {
    lost: false,
    sent: [],
    q: [...packets],
    send(bytes) { this.sent.push(bytes); return true; },
    async read() { return this.q.shift() || null; },
    async close() {},
  };
}

test("a stock update loader can finish the official package", () => {
  assert.equal(loaderAccepts("ota-FM-1_015", "FM-1_016"), true);
  assert.equal(loaderAccepts("ota-FM-1_969", "FM-1_969"), true);
  assert.equal(loaderAccepts("ota-FM-1_969", "FM-1_016"), false);
  assert.equal(loaderAccepts("ota-FM-1_713", "FM-1_096"), false);
});

test("the official loader's downward read keeps the progress moving", () => {
  const image = imageWithFlash(0x100, 0x9000);
  assert.deepEqual(flashSpan(image), {off: 0x100, size: 0x9000});
  const meter = new WriteMeter(image);
  for (let i = 0; i < 8; i++) assert.equal(meter.add(0x100 + 0x8000 - i * 0x200, 0x200), 0);
  let prev = 0;
  for (let i = 8; i < 12; i++) {
    const pct = meter.add(0x100 + 0x8000 - i * 0x200, 0x200);
    assert.ok(pct > prev);
    prev = pct;
  }
});

test("a check address during the write does not end the update", async () => {
  const link = fakeLink([
    request(0, 16),
    request(0xE0000000, 8),
    request(16, 16, 9),
    request(0xF0000000, 8),
  ]);
  const result = await new Updater(null, null, {settle: 0}).serve(link, new Uint8Array(64), 0xF0000000, 200, () => {});
  assert.equal(result.finished, true);
  assert.equal(result.served, 2);
  assert.equal(link.sent.length, 4);
});

test("a restarted stock loader is picked up and the write finishes", async () => {
  const updater = new Updater(null, null, {settle: 0, idle: 40, resumeMax: 2, resumeWait: 20});
  updater.find = async () => ({
    id: {text: "ota-FM-1_015", model: "ota-FM-1"},
    link: fakeLink([request(0xF0000000, 8)]),
  });
  const result = await updater.writeResuming({
    id: {text: "ota-FM-1_015"},
    link: fakeLink([request(0, 8)]),
  }, new Uint8Array(32), () => {}, "FM-1_016");
  assert.equal(result.finished, true);
});

test("the official package is not written by another firmware's loader", async () => {
  let sent = false;
  await assert.rejects(() => new Updater(null).write({
    id: {text: "ota-FM-1_969"},
    link: {send() { sent = true; return true; }, async close() {}},
  }, new Uint8Array(8), () => {}, "FM-1_016"), /matching package/);
  assert.equal(sent, false);
});

test("the page can see an FM-1 that introduces itself as Fmdac", () => {
  assert.match("Fmdac", FM1_PORT);
  assert.match("FM-1", FM1_PORT);
  assert.doesNotMatch("Launchpad", FM1_PORT);
});
