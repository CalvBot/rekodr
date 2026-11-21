// Builds the installable Stream Deck plugin: node streamdeck\pack.mjs
// Checks the manifest the way Stream Deck and the Marketplace do, then zips the .sdPlugin folder into
// streamdeck\build\dev.calvbot.clipback.streamDeckPlugin. Double-clicking that file installs the plugin.
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";

const UUID = "dev.calvbot.clipback";
const root = path.join(import.meta.dirname, UUID + ".sdPlugin");
const out = path.join(import.meta.dirname, "build", UUID + ".streamDeckPlugin");
const manifest = JSON.parse(fs.readFileSync(path.join(root, "manifest.json"), "utf8"));

const problems = [];
const need = (ok, what) => { if (!ok) problems.push(what); };
const has = rel => fs.existsSync(path.join(root, rel));
// an image is named without its extension; Stream Deck looks for .svg or .png (plus @2x for PNG)
const image = (rel, what, pngOnly = false) => {
    const exts = pngOnly ? [".png"] : [".svg", ".png"];
    need(exts.some(e => has(rel + e)), `${what}: no ${rel}${exts.join(" or ")}`);
    if (has(rel + ".png") && !has(rel + ".svg")) need(has(rel + "@2x.png"), `${what}: no ${rel}@2x.png`);
};

for (const key of ["Actions", "Author", "CodePath", "Description", "Icon", "Name", "OS", "SDKVersion", "Software", "UUID", "Version"])
    need(manifest[key] !== undefined, `manifest: no ${key}`);
need(manifest.UUID === UUID, `manifest: UUID is ${manifest.UUID}, the folder says ${UUID}`);
need(/^[a-z0-9.-]+$/.test(UUID), "UUID must be lowercase letters, digits, dots and dashes");
need(/^\d+\.\d+\.\d+\.\d+$/.test(manifest.Version ?? ""), "manifest: Version must be four numbers");
need(!/plugin|stream ?deck/i.test(manifest.Name ?? ""), "manifest: the Name may not say Plugin or Stream Deck");
need(has(manifest.CodePath ?? ""), `manifest: no CodePath file ${manifest.CodePath}`);
if (manifest.PropertyInspectorPath) need(has(manifest.PropertyInspectorPath), `manifest: no ${manifest.PropertyInspectorPath}`);
// Node 24 needs Stream Deck 7.1; older Stream Deck only runs Node 20
const node = Number(manifest.Nodejs?.Version), sd = Number(manifest.Software?.MinimumVersion);
need(node === 20 || (node === 24 && sd >= 7.1), `manifest: Node ${node} with Stream Deck ${sd} won't load`);
image(manifest.Icon, "plugin icon", true);
if (manifest.CategoryIcon) image(manifest.CategoryIcon, "category icon");
for (const a of manifest.Actions ?? []) {
    need(a.UUID?.startsWith(UUID + "."), `action ${a.UUID}: UUID must start with ${UUID}.`);
    image(a.Icon, `action ${a.UUID} icon`);
    for (const s of a.States ?? []) image(s.Image, `action ${a.UUID} key image`);
}
if (problems.length) {
    for (const p of problems) console.error(p);
    process.exit(1);
}

// every file in the folder, under <uuid>.sdPlugin/ with forward slashes
const files = [];
const walk = rel => {
    for (const e of fs.readdirSync(path.join(root, rel), { withFileTypes: true })) {
        const r = rel ? rel + "/" + e.name : e.name;
        if (e.isDirectory()) walk(r);
        else files.push(r);
    }
};
walk("");
files.sort();

// a plain zip: deflated entries, a central directory, no zip64 (the plugin is far below 4 GB)
const parts = [], central = [];
let offset = 0;
for (const rel of files) {
    const data = fs.readFileSync(path.join(root, rel));
    const packed = zlib.deflateRawSync(data, { level: 9 });
    const name = Buffer.from(`${UUID}.sdPlugin/${rel}`, "utf8");
    const crc = zlib.crc32(data);
    const head = (sig, size) => {
        const b = Buffer.alloc(size);
        b.writeUInt32LE(sig, 0);
        return b;
    };
    const local = head(0x04034b50, 30);
    local.writeUInt16LE(20, 4);              // version needed
    local.writeUInt16LE(0x0800, 6);          // names are UTF-8
    local.writeUInt16LE(8, 8);               // deflate
    local.writeUInt16LE(0, 10);              // time
    local.writeUInt16LE((2026 - 1980) << 9 | 1 << 5 | 1, 12); // a fixed date, so the same files make the same zip
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(packed.length, 18);
    local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(name.length, 26);
    parts.push(local, name, packed);
    const dirent = head(0x02014b50, 46);
    dirent.writeUInt16LE(20, 4);             // made by
    local.copy(dirent, 6, 4, 30);            // version needed .. name length
    dirent.writeUInt32LE(offset, 42);
    central.push(dirent, name);
    offset += local.length + name.length + packed.length;
}
const dirSize = central.reduce((n, b) => n + b.length, 0);
const end = Buffer.alloc(22);
end.writeUInt32LE(0x06054b50, 0);
end.writeUInt16LE(files.length, 8);
end.writeUInt16LE(files.length, 10);
end.writeUInt32LE(dirSize, 12);
end.writeUInt32LE(offset, 16);

fs.mkdirSync(path.dirname(out), { recursive: true });
fs.writeFileSync(out, Buffer.concat([...parts, ...central, end]));
console.log(`${path.relative(process.cwd(), out)}: ${files.length} files, ${fs.statSync(out).size} bytes`);
