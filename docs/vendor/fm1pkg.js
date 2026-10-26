// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
// Selected package identity/image helpers from Skiff v1.3.2.
const BLOCKS = 20, BLK = 0x30, KEEP = 0x2F;
// the package identity ("FM-1_904"): one marker byte after each of the first 20 blocks
export function productOf(fwsc) {
  if (fwsc.length < BLOCKS * BLK) throw new Error("not an FM-1 package (too short)");
  let s = "";
  for (let i = 0; i < BLOCKS; i++) {
    const m = fwsc[i * BLK + KEEP];
    if (m !== 0x7D) s += String.fromCharCode((m - i - 1) & 0xFF);
  }
  return s;
}

function jlEnc(bytes) {
  const out = new Uint8Array(bytes);
  let key = 0xFFFF;
  for (let i = 0; i < out.length; i++) {
    out[i] ^= key & 0xFF;
    key = ((key << 1) ^ (key & 0x8000 ? 0x1021 : 0)) & 0xFFFF;
  }
  return out;
}

function rd(bytes, off, n) {
  let v = 0;
  for (let i = 0; i < n; i++) v += bytes[off + i] * 2 ** (8 * i);
  return v >>> 0;
}

// where flash.bin lies in a logical image, or null when the file list is absent
export function flashSpan(img) {
  if (!img || img.length < 0x40) return null;
  const count = rd(jlEnc(img.subarray(0, 0x40)), 8, 2);
  for (let i = 0; i < Math.min(count, 8) && 0x90 + i * 0x50 <= img.length; i++) {
    const entry = jlEnc(img.subarray(0x40 + i * 0x50, 0x90 + i * 0x50));
    const off = rd(entry, 8, 4);
    const size = rd(entry, 12, 4);
    if (rd(entry, 0, 2) === 0 && size && off + size <= img.length) return {off, size};
  }
  return null;
}

// the image the device addresses during an update: the .fwsc without the 20 marker bytes
export function logicalImage(fwsc) {
  const out = new Uint8Array(fwsc.length - BLOCKS);
  for (let i = 0; i < BLOCKS; i++) out.set(fwsc.subarray(i * BLK, i * BLK + KEEP), i * KEEP);
  out.set(fwsc.subarray(BLOCKS * BLK), BLOCKS * KEEP);
  return out;
}

export async function sha256hex(bytes) {
  const d = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(d)].map((x) => x.toString(16).padStart(2, "0")).join("");
}
