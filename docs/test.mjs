// SPDX-License-Identifier: GPL-3.0-only
import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import test from "node:test";
import {FM1_PORT} from "./vendor/fm1ota.js";
import {productOf, logicalImage} from "./vendor/fm1pkg.js";
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
  assert.equal(catalog.current.product, "FM-1_969");
  assert.equal(catalog.current.filename, "fmdac.fwsc");
  assert.equal(catalog.recovery.length, 3);
  assert.deepEqual(catalog.recovery.map((release) => release.product), ["FM-1_713", "FM-1_900", "FM-1_016"]);
  const bytes = new Uint8Array(await readFile(new URL("./fmdac.fwsc", import.meta.url)));
  const checked = await validatePinned(bytes, catalog.current);
  assert.equal(checked.product, "FM-1_969");
  assert.equal(productOf(bytes), "FM-1_969");
  assert.equal(logicalImage(bytes).length, bytes.length - 20);
});

test("the page can see an FM-1 that introduces itself as Fmdac", () => {
  assert.match("Fmdac", FM1_PORT);
  assert.match("FM-1", FM1_PORT);
  assert.doesNotMatch("Launchpad", FM1_PORT);
});
