// SPDX-License-Identifier: GPL-3.0-only
import {Updater} from "./vendor/fm1ota.js";
import {acceptPackage, assertCatalog, blockReason, loaderMismatch} from "./packages.js";

const $ = (id) => document.getElementById(id);
const supported = window.isSecureContext && typeof navigator.requestMIDIAccess === "function" && !!crypto.subtle;

let catalog = null;
let access = null;
let device = null;
let prepared = null;
let review = null;
let mode = "install";
let busy = false;
let updating = false;
let epoch = 0;

const selectedRecovery = () => catalog?.recovery.find((release) => release.id === $("firmware").value) ?? null;
const targetRelease = () => (mode === "recover" ? selectedRecovery() : catalog?.current ?? null);

function log(message) {
  const line = `${new Date().toLocaleTimeString()}  ${message}\n`;
  $("log").textContent += line;
  if ($("log").textContent.length > 24000) $("log").textContent = $("log").textContent.slice(-20000);
}

function deviceMessage(message, error = false) {
  $("device-status").textContent = message;
  $("device-status").classList.toggle("error", error);
}

function reasonNow() {
  return blockReason({
    catalogReady: !!catalog,
    supported,
    mode,
    release: targetRelease(),
    prepared,
    device,
  });
}

function installLabel() {
  if (!prepared) return "Choose a firmware file";
  if (!supported) return "Use Chrome or Edge";
  if (!device) return "Connect the FM-1";
  if (loaderMismatch(device.identity, prepared.product)) return "Choose the matching file";
  if (device.identity.startsWith("ota-")) return "Finish the update";
  if (mode === "install") return "Install Fmdac";
  return `Install ${targetRelease()?.name || prepared.product}`;
}

function showDetails(name, product, hash) {
  $("package-name").textContent = name || "—";
  $("package-product").textContent = product || "—";
  $("package-hash").textContent = hash || "—";
}

function render() {
  const release = targetRelease();
  $("current-panel").hidden = mode !== "install";
  $("recovery-panel").hidden = mode !== "recover";
  $("mode-install").setAttribute("aria-pressed", String(mode === "install"));
  $("mode-recover").setAttribute("aria-pressed", String(mode === "recover"));
  for (const id of ["mode-install", "mode-recover", "firmware", "firmware-file"]) {
    $(id).disabled = busy || !catalog;
  }
  $("connect").disabled = busy || !supported || !catalog;
  $("connect").textContent = device ? "Reconnect" : "Connect";
  $("install").textContent = installLabel();
  const reason = reasonNow();
  $("install").disabled = busy || !!reason;
  $("action-help").textContent = reason || `Ready to write ${prepared.product} to ${device.name}. You confirm on the next step, before the update starts.`;
  $("action-help").classList.toggle("success", !reason);
  $("action-help").classList.toggle("error", false);
  if (mode === "recover" && release) {
    $("firmware-description").textContent = release.description || "";
    $("firmware-download").href = release.download;
    $("firmware-download").textContent = `Get ${release.filename}`;
  }
}

function clearPackage() {
  epoch += 1;
  prepared = null;
  review = null;
  if ($("confirm-dialog").open) $("confirm-dialog").close();
  $("firmware-file").value = "";
  $("package-status").className = "";
  $("package-details").open = false;
  const release = targetRelease();
  if (mode === "recover" && release?.enabled) {
    $("package-status").textContent = `Choose ${release.filename}. The page checks it against the published checksum.`;
    showDetails(release.filename, release.product, release.sha256);
  } else {
    $("package-status").textContent = "Choose a .fwsc file. Its identity is read on this computer.";
    showDetails("—", "—", "—");
  }
  render();
}

function remember(checked, filename) {
  prepared = {...checked, filename};
  $("package-status").textContent = `${checked.product} · ${filename} · checksum read`;
  $("package-status").className = "valid";
  $("package-details").open = true;
  showDetails(filename, checked.product, checked.digest);
  log(`Checked ${filename}: ${checked.product}, ${checked.digest}.`);
}

async function takeFile(file) {
  if ($("confirm-dialog").open) $("confirm-dialog").close();
  review = null;
  const release = targetRelease();
  const pinned = mode === "recover" || catalog.current.enabled === true;
  const token = epoch + 1;
  epoch = token;
  prepared = null;
  $("package-status").className = "";
  $("package-status").textContent = "Checking the package…";
  render();
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    if (token !== epoch) return;
    const checked = await acceptPackage(bytes, release, pinned);
    if (token !== epoch) return;
    remember(checked, file.name);
  } catch (error) {
    if (token !== epoch) return;
    prepared = null;
    $("package-status").textContent = error.message;
    $("package-status").className = "error";
    $("package-details").open = false;
    const release = targetRelease();
    if (mode === "recover" && release?.enabled) showDetails(release.filename, release.product, release.sha256);
    else showDetails(file.name, "—", "—");
    log(`Package rejected: ${error.message}`);
  }
  if (token === epoch) render();
}

async function loadBundled() {
  if (mode !== "install" || !catalog) return;
  const token = epoch;
  let response;
  try {
    response = await fetch("./fmdac.fwsc", {cache: "no-cache"});
  } catch {
    return;
  }
  if (!response.ok || token !== epoch || mode !== "install") return;
  const type = response.headers.get("content-type") || "";
  if (type.includes("text/html")) return;
  try {
    const bytes = new Uint8Array(await response.arrayBuffer());
    if (token !== epoch || mode !== "install") return;
    const checked = await acceptPackage(bytes, catalog.current, catalog.current.enabled === true);
    if (token !== epoch || mode !== "install") return;
    remember(checked, "fmdac.fwsc");
    $("package-status").textContent = `Loaded fmdac.fwsc from this page · ${checked.product}`;
  } catch (error) {
    if (token !== epoch) return;
    log(`The bundled fmdac.fwsc was left unused: ${error.message}`);
  }
  if (token === epoch) render();
}

for (const choice of ["install", "recover"]) {
  $(`mode-${choice}`).addEventListener("click", async () => {
    if (busy || !catalog || mode === choice) return;
    mode = choice;
    clearPackage();
    if (choice === "install") await loadBundled();
  });
}

$("firmware").addEventListener("change", () => {
  if (!busy) clearPackage();
});
$("firmware-file").addEventListener("change", (event) => {
  const file = event.target.files?.[0];
  if (!busy && file) takeFile(file);
});

$("connect").addEventListener("click", async () => {
  if (busy || !supported) return;
  busy = true;
  device = null;
  render();
  deviceMessage("Waiting for MIDI permission and an FM-1 identity…");
  try {
    if (!access?.sysexEnabled) access = await navigator.requestMIDIAccess({sysex: true});
    if (!access.sysexEnabled) throw new Error("Allow MIDI SysEx for this site in Chrome or Edge, then connect again.");
    access.onstatechange = (event) => {
      if (!busy && device && event.port?.state === "disconnected" && device.ports.includes(event.port.id)) {
        device = null;
        deviceMessage("The FM-1 disconnected. Connect it again.");
        render();
      }
    };
    const found = await new Updater(access, log).find();
    if (!found) throw new Error("No FM-1 answered. Use a data cable, close other MIDI apps, and read Finish or recover if the computer shows WL80UBOOT.");
    device = {
      identity: found.id.text,
      name: found.name,
      ports: [found.link.input.id, found.link.output.id],
    };
    await found.link.close();
    const updatingNow = device.identity.startsWith("ota-") ? " · update mode" : "";
    deviceMessage(`Connected: ${device.name} · ${device.identity}${updatingNow}`);
    log(`Identity ${device.identity} on ${device.name}.`);
  } catch (error) {
    const denied = error.name === "NotAllowedError" || error.name === "SecurityError";
    deviceMessage(denied ? "MIDI permission was denied. Allow MIDI SysEx for this site, then connect again." : error.message, true);
    log(`Connection: ${error.message}`);
  } finally {
    busy = false;
    render();
  }
});

function closeReview() {
  review = null;
  if ($("confirm-dialog").open) $("confirm-dialog").close();
}

$("install").addEventListener("click", () => {
  if (busy || reasonNow() || !prepared || !device) return;
  review = {
    filename: prepared.filename,
    product: prepared.product,
    digest: prepared.digest,
    identity: device.identity,
    ports: [...device.ports],
    mode,
    epoch,
  };
  $("confirm-description").textContent = `Write ${prepared.product} to this FM-1?`;
  $("confirm-device").textContent = `${device.name} · ${device.identity}`;
  $("confirm-file").textContent = `${prepared.filename} · ${prepared.product}`;
  $("confirm-hash").textContent = prepared.digest;
  $("confirm-dialog").showModal();
});

$("confirm-cancel").addEventListener("click", closeReview);
$("confirm-dialog").addEventListener("cancel", () => {
  review = null;
});

function transferStep(stage, detail) {
  const labels = {
    start: "Preparing the update",
    verify: "The FM-1 is checking the package",
    loader: "Waiting for update mode",
    write: "Writing firmware. Keep the cable connected.",
    reboot: "Waiting for the FM-1 to restart",
    done: "The FM-1 came back with the expected identity",
  };
  $("transfer-stage").textContent = labels[stage] || stage;
  let value = {start: 3, verify: 8, loader: 12, reboot: 96, done: 100}[stage];
  if (stage === "write") {
    const end = detail?.end || 0;
    value = 15 + Math.min(1, end / prepared.image.length) * 78;
  }
  $("progress").value = Math.max(Number($("progress").value) || 0, value || 0);
  $("transfer-percent").textContent = `${Math.floor($("progress").value)}%`;
  $("transfer-help").textContent = stage === "done"
    ? `The FM-1 reports ${detail}. Check the screen.`
    : "Keep the cable connected and the computer awake. If this stops, use this same file to finish.";
  if (stage !== "verify" && stage !== "write") log(`Stage ${stage}${typeof detail === "string" ? ` · ${detail}` : ""}`);
}

$("confirm-install").addEventListener("click", async () => {
  const confirmed = review;
  if (!confirmed || busy || !prepared || !device || confirmed.epoch !== epoch || confirmed.mode !== mode ||
      confirmed.digest !== prepared.digest || confirmed.identity !== device.identity || reasonNow()) {
    closeReview();
    render();
    return;
  }
  review = null;
  $("confirm-dialog").close();
  busy = true;
  updating = true;
  render();
  $("progress").value = 0;
  $("transfer-stage").className = "";
  $("transfer-help").textContent = "Starting. Leave this tab open.";
  try {
    const updater = new Updater(access, log);
    if (confirmed.identity.startsWith("ota-")) {
      const finished = await updater.resume(prepared.image, transferStep, prepared.product, confirmed.ports);
      if (!finished) throw new Error("The update did not finish. Connect again and finish it with this same file.");
    } else {
      await updater.install(prepared.image, prepared.product, transferStep, confirmed.identity, confirmed.ports);
    }
    $("transfer-stage").className = "valid";
    deviceMessage("The update finished. Connect again before another install.");
    log("Update finished.");
  } catch (error) {
    $("transfer-stage").textContent = "The update stopped";
    $("transfer-stage").className = "error";
    $("transfer-help").textContent = `${error.message} Keep this file. If the FM-1 is in update mode, connect and finish this same package.`;
    deviceMessage(error.message, true);
    log(`Update stopped${error.code ? ` (${error.code})` : ""}: ${error.message}`);
  } finally {
    device = null;
    busy = false;
    updating = false;
    render();
  }
});

window.addEventListener("beforeunload", (event) => {
  if (!updating) return;
  event.preventDefault();
  event.returnValue = "";
});

$("save-log").addEventListener("click", () => {
  const url = URL.createObjectURL(new Blob([$("log").textContent || "No device operations recorded.\n"], {type: "text/plain"}));
  const link = Object.assign(document.createElement("a"), {href: url, download: "fmdac-installer-log.txt"});
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});

if (!supported) {
  $("browser-note").hidden = false;
  $("browser-note").textContent = "This browser can check a package, and it cannot connect to the FM-1. Open the page in Chrome or Edge on a computer.";
}

async function init() {
  try {
    const response = await fetch("./releases.json", {cache: "no-cache"});
    if (!response.ok) throw new Error("The firmware list could not be loaded. Reload this page.");
    const data = assertCatalog(await response.json());
    catalog = data;
    $("current-title").textContent = data.current.name;
    $("current-badge").textContent = data.current.enabled === true ? "Package published" : "No package published";
    $("current-badge").classList.toggle("ready", data.current.enabled === true);
    $("current-reason").textContent = data.current.enabled === true
      ? `Fmdac ${data.current.version} is published as ${data.current.filename}.`
      : data.current.reason;
    $("current-source").href = data.current.source || "https://github.com/chinotto1/Fmdac";
    $("firmware").replaceChildren(...data.recovery.map((release) => new Option(`${release.name} ${release.version}`, release.id)));
    clearPackage();
    log("Firmware list loaded.");
    await loadBundled();
  } catch (error) {
    $("current-reason").textContent = error.message;
    $("current-badge").textContent = "Firmware list unavailable";
    $("action-help").textContent = error.message;
    $("action-help").classList.add("error");
    log(error.message);
  }
}

init();
