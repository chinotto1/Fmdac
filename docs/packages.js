// SPDX-License-Identifier: GPL-3.0-only
import {productOf, logicalImage, sha256hex} from "./vendor/fm1pkg.js";

export const MAX_PACKAGE_BYTES = 2 * 1024 * 1024;

export function isProduct(product) {
  return /^FM-1_\d{3}$/.test(product || "");
}

export function checkCatalogEntry(release) {
  if (release?.enabled !== true) {
    throw new Error(release?.reason || "This firmware has no installable package.");
  }
  if (!isProduct(release.product) || !/^[a-f0-9]{64}$/.test(release.sha256 || "") ||
      !Number.isSafeInteger(release.size) || release.size < 960 || release.size > MAX_PACKAGE_BYTES ||
      typeof release.filename !== "string" || !release.filename.endsWith(".fwsc")) {
    throw new Error("The firmware list is incomplete. No update can start.");
  }
  return release;
}

export function assertCatalog(data) {
  if (!data?.current || typeof data.current.name !== "string" || !Array.isArray(data.recovery) || !data.recovery.length) {
    throw new Error("The firmware list is invalid. No update can start.");
  }
  if (data.current.enabled === true) checkCatalogEntry(data.current);
  else if (typeof data.current.reason !== "string" || !data.current.reason.trim()) {
    throw new Error("The firmware list is invalid. No update can start.");
  }
  const ids = new Set();
  for (const release of data.recovery) {
    checkCatalogEntry(release);
    if (ids.has(release.id)) throw new Error("The firmware list is invalid. No update can start.");
    ids.add(release.id);
  }
  return data;
}

export async function inspectPackage(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.length > MAX_PACKAGE_BYTES) {
    throw new Error("This file is too large for an FM-1 update package.");
  }
  let product;
  try {
    product = productOf(bytes);
  } catch {
    throw new Error("This file is not an FM-1 package.");
  }
  if (!isProduct(product)) throw new Error("This file has no FM-1 package identity.");
  return {image: logicalImage(bytes), product, digest: await sha256hex(bytes), release: null};
}

export async function validatePinned(bytes, release) {
  checkCatalogEntry(release);
  if (!(bytes instanceof Uint8Array) || bytes.length > MAX_PACKAGE_BYTES) {
    throw new Error("This file is too large for an FM-1 update package.");
  }
  let product;
  try {
    product = productOf(bytes);
  } catch {
    throw new Error("This file is not an FM-1 package.");
  }
  if (product !== release.product) throw new Error(`This package is ${product}. Choose ${release.filename}.`);
  if (bytes.length !== release.size) {
    throw new Error(`Choose the original ${release.filename}. The file size does not match the published package.`);
  }
  const digest = await sha256hex(bytes);
  if (digest !== release.sha256) {
    throw new Error(`Checksum mismatch. Download ${release.filename} again from its publisher.`);
  }
  return {image: logicalImage(bytes), product, digest, release};
}

export async function acceptPackage(bytes, release, pinned) {
  return pinned ? validatePinned(bytes, release) : inspectPackage(bytes);
}

export function loaderMismatch(identity, product) {
  return Boolean(identity?.startsWith("ota-") && identity !== `ota-${product}`);
}

export function blockReason({catalogReady, supported, mode, release, prepared, device, backupSaved}) {
  if (!catalogReady) return "Loading the firmware list…";
  if (mode === "recover") {
    if (!release) return "Choose a firmware.";
    if (release.enabled !== true) return release.reason || "This package is not available.";
  }
  if (!prepared) {
    return mode === "install"
      ? "Choose a .fwsc file. Its identity is read on this computer before anything is sent."
      : `Download ${release.filename} and choose it here. The page checks the published checksum.`;
  }
  if (!supported) return "Open this page in Chrome or Edge on a computer to connect the FM-1.";
  if (!device) return "Connect the FM-1 so this page can see what is running.";
  if (loaderMismatch(device.identity, prepared.product)) {
    return `The FM-1 is finishing an update of ${device.identity.replace(/^ota-/, "")}. Choose that same package. This file is ${prepared.product}.`;
  }
  if (!backupSaved) return "Tick the box once you can get back to the firmware on the FM-1 now.";
  return "";
}
