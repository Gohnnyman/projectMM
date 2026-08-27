// The backup/restore rename map (src/ui/migrate.js): every schema break MIGRATING.md
// records, applied client-side to a backup before upload. These tests are the functional
// documentation of what a restore rescues, what it flags for review, and what it never touches.
// Run: `node --test test/js/migrate.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";

import { applyMigrations } from "../../src/ui/migrate.js";

test("renamed config files land under their new names, reported", () => {
    const { files, report } = applyMigrations({
        "/.config/Layers.json": JSON.stringify({ enabled: true }),
        "/.config/DriverGroup.json": JSON.stringify({ enabled: true }),
    });
    assert.ok(files["/.config/Effects.json"]);
    assert.ok(files["/.config/Drivers.json"]);
    assert.equal(files["/.config/Layers.json"], undefined);
    assert.ok(report.some(r => r.kind === "renamed" && r.detail.includes("Effects.json")));
});

test("renamed module types map in place, including nested children", () => {
    const cfg = { "0.type": "Layers", "0.0.type": "ParlioLedDriver", "0.0.enabled": true };
    const { files, report } = applyMigrations({ "/.config/Effects.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/Effects.json"]);
    assert.equal(out["0.type"], "Effects");
    assert.equal(out["0.0.type"], "ParallelLedDriver");
    // Parlio kept its peripheral value, so the map SETS it rather than asking for review.
    assert.equal(out["0.0.peripheral"], "Parlio");
    assert.ok(report.filter(r => r.kind === "renamed").length >= 2);
});

test("chip-dependent peripherals get a review entry, never a guessed value", () => {
    const cfg = { "0.type": "I80LedDriver", "0.pin0": 16 };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/Drivers.json"]);
    assert.equal(out["0.type"], "ParallelLedDriver");
    assert.equal(out["0.peripheral"], undefined);   // not guessed
    assert.ok(report.some(r => r.kind === "review" && r.detail.includes("re-picked")));
});

test("a deterministic peripheral move is set: MoonLedDriver becomes ParallelLedDriver on LCD-MM", () => {
    const cfg = { "0.type": "MoonLedDriver", "0.useRing": true };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/Drivers.json"]);
    assert.equal(out["0.type"], "ParallelLedDriver");
    assert.equal(out["0.peripheral"], "LCD-MM");
    assert.ok(report.some(r => r.kind === "review" && r.detail.includes("S3/P4 only")));
});

test("control VALUES migrate too: peripheral MoonI80 maps to LCD-MM, i80 flags review", () => {
    const cfg = { "0.type": "ParallelLedDriver", "0.peripheral": "MoonI80",
                  "1.type": "ParallelLedDriver", "1.peripheral": "i80" };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/Drivers.json"]);
    assert.equal(out["0.peripheral"], "LCD-MM");                 // deterministic: mapped
    assert.equal(out["1.peripheral"], "i80");                    // chip-dependent: untouched...
    assert.ok(report.some(r => r.kind === "review" && r.where.includes("1.peripheral")));   // ...but flagged
    assert.ok(report.some(r => r.kind === "renamed" && r.detail.includes("MoonI80 → LCD-MM")));
});

test("control renames apply, and semantics changes flag review instead of guessing", () => {
    const cfg = { "0.type": "PreviewDriver", "0.fps": 30, "1.type": "MoonLedDriver", "1.forceRing": 2 };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/Drivers.json"]);
    assert.equal(out["0.targetFps"], 30);
    assert.equal(out["0.fps"], undefined);
    assert.equal(out["1.useRing"], 2);              // name maps (scope follows the type rename); value untouched...
    assert.ok(report.some(r => r.kind === "review" && r.where.includes("useRing")));   // ...but flagged
});

test("renames are scoped by module type: NetworkSendDriver keeps its own fps", () => {
    // The bench-found bug: fps → targetFps belongs to PreviewDriver alone; other drivers
    // legitimately own an fps control today and a blanket rename corrupts them.
    const cfg = { "0.type": "NetworkSendDriver", "0.fps": 60, "1.type": "PreviewDriver", "1.fps": 24 };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/Drivers.json"]);
    assert.equal(out["0.fps"], 60);                 // untouched
    assert.equal(out["1.targetFps"], 24);           // renamed
    assert.equal(report.length, 1);
});

test("preset payload values rename (the captured container), other content byte-exact", () => {
    const preset = '{"container":"Layers","role":"layer","x":1}';
    const script = "let x = 1; //, untouched, not JSON, not a preset";
    const { files, report } = applyMigrations({
        "/.config/presets/p1.json": preset,
        "/scripts/anim.mle": script,
    });
    assert.equal(files["/.config/presets/p1.json"], '{"container":"Effects","role":"effects","x":1}');
    assert.equal(files["/scripts/anim.mle"], script);   // byte-exact passthrough
    assert.ok(report.some(r => r.detail.includes("preset value")));
});

test("unknown content passes through untouched with an empty report", () => {
    const cfg = JSON.stringify({ modern: true, "0.type": "NoiseEffect", "0.speed": 128 });
    const { files, report } = applyMigrations({ "/.config/Effects.json": cfg });
    assert.deepEqual(JSON.parse(files["/.config/Effects.json"]), JSON.parse(cfg));
    assert.equal(report.length, 0);
});

test("a corrupt config file is restored as-is and flagged for review", () => {
    const { files, report } = applyMigrations({ "/.config/Broken.json": "{not json" });
    assert.equal(files["/.config/Broken.json"], "{not json");
    assert.ok(report.some(r => r.kind === "review" && r.detail.includes("not valid JSON")));
});
