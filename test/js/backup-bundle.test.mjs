// The backup/restore engine core (src/ui/migrate.js): the filesystem walk that builds a
// bundle, the dirs-before-files restore ordering, and the restore report that names what an old
// config loses on new firmware. Pure functions with injected I/O, so these run with mocks.
// Run: `node --test test/js/backup-bundle.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";

import { collectFiles, restoreDirs, diffRestore } from "../../src/ui/migrate.js";

const tree = {
    "/": [{ name: ".config", isDir: true, size: 0 }, { name: "scripts", isDir: true, size: 0 }],
    "/.config": [{ name: "Network.json", isDir: false, size: 15 },
                 { name: "presets", isDir: true, size: 0 }],
    "/.config/presets": [{ name: "p1.json", isDir: false, size: 9 }],
    "/scripts": [{ name: "a.mle", isDir: false, size: 7 }],
};
const content = {
    "/.config/Network.json": '{"ssid":"abc"}\n',
    "/.config/presets/p1.json": '{"x":123}',
    "/scripts/a.mle": "let x=1",
};

test("the walk collects every file at every depth, byte-verified", async () => {
    const { files, skipped } = await collectFiles(async d => tree[d] || [], async p => content[p]);
    assert.deepEqual(Object.keys(files).sort(), Object.keys(content).sort());
    assert.equal(files["/scripts/a.mle"], "let x=1");
    assert.deepEqual(skipped, []);
});

test("a non-text file is skipped and named, not archived mangled", async () => {
    // Reading binary as UTF-8 inflates it (replacement characters), so bytes > listed size.
    const t = { ...tree, "/scripts": [...tree["/scripts"], { name: "fw.bin", isDir: false, size: 3 }] };
    const c = { ...content, "/scripts/fw.bin": "\uFFFD\uFFFD\uFFFD" };   // 9 bytes for a 3-byte file
    const { files, skipped } = await collectFiles(async d => t[d] || [], async p => c[p]);
    assert.deepEqual(skipped, ["/scripts/fw.bin"]);
    assert.equal(files["/scripts/fw.bin"], undefined);
    assert.deepEqual(Object.keys(files).sort(), Object.keys(content).sort());   // the rest intact
});

test("a truncated read fails the backup loudly instead of archiving it", async () => {
    const bad = { ...content, "/scripts/a.mle": "let " };   // 4 bytes, listing says 7
    await assert.rejects(
        collectFiles(async d => tree[d] || [], async p => bad[p]),
        /truncated/);
});

test("restore creates parent directories before children, each once", () => {
    const dirs = restoreDirs({
        "/.config/presets/p1.json": "", "/.config/Network.json": "", "/scripts/a.mle": "",
    });
    assert.deepEqual(dirs, ["/.config", "/scripts", "/.config/presets"]);
});

const liveState = [
    { type: "NetworkModule", controls: [{ name: "ssid" }, { name: "password" }] },
    { type: "Effects", controls: [{ name: "enabled" }],
      children: [{ type: "NoiseEffect", controls: [{ name: "speed" }] }] },
];

test("the report names a module type this firmware no longer has", () => {
    const r = diffRestore({ "/.config/OldDriver.json": '{"a":1}' }, liveState);
    assert.equal(r.length, 1);
    assert.equal(r[0].kind, "module");
    assert.match(r[0].detail, /OldDriver/);
});

test("the report names a control that no longer exists, defaults noted", () => {
    const cfg = JSON.stringify({ ssid: "x", oldKnob: 3 });
    const r = diffRestore({ "/.config/NetworkModule.json": cfg }, liveState);
    assert.equal(r.length, 1);
    assert.equal(r[0].kind, "control");
    assert.match(r[0].detail, /oldKnob/);
});

test("nested child chains resolve through their N.type keys, unknown children reported once", () => {
    const cfg = JSON.stringify({
        enabled: true,
        "0.type": "NoiseEffect", "0.speed": 5,
        "1.type": "GoneEffect", "1.a": 1, "1.b": 2,
    });
    const r = diffRestore({ "/.config/Effects.json": cfg }, liveState);
    assert.equal(r.length, 1);                       // GoneEffect once, not per key
    assert.equal(r[0].kind, "module");
    assert.match(r[0].detail, /GoneEffect/);
});

test("a fully modern config yields an empty report; preset payloads are skipped", () => {
    const r = diffRestore({
        "/.config/NetworkModule.json": '{"ssid":"x","password":"y"}',
        "/.config/presets/p1.json": '{"whatever":"Layers"}',
    }, liveState);
    assert.deepEqual(r, []);
});

test("a registered type not yet in the live tree is not reported: it instantiates at reboot", () => {
    const files = { "/.config/Services.json": JSON.stringify({ "0.type": "AudioService", "0.gain": 5 }) };
    const state = [{ type: "Services", controls: [], children: [] }];
    assert.deepEqual(diffRestore(files, state, ["Services", "AudioService"]), []);
    // without the registry entry the same file reports the missing child
    const rep = diffRestore(files, state, ["Services"]);
    assert.equal(rep.length, 1);
    assert.match(rep[0].detail, /AudioService does not exist/);
});

