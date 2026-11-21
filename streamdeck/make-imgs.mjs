// Writes the plugin's static images (imgs\) from the same drawing code the keys use:
//   node streamdeck\make-imgs.mjs
// *-key.svg is what a key shows before the plugin has drawn it; the others are the white icons in Stream Deck's
// action list, and the plugin's own icon. The plugin icon has to be PNG (256 and 512 px): headless Edge draws it on
// a canvas, with a profile of its own in a temp folder.
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { C, card, saveKey, history, film, dot } from "./dev.calvbot.clipback.sdPlugin/render.mjs";

const dir = path.join(import.meta.dirname, "dev.calvbot.clipback.sdPlugin", "imgs");
fs.mkdirSync(dir, { recursive: true });
const write = (name, svg) => fs.writeFileSync(path.join(dir, name + ".svg"), svg + "\n");
const small = body => `<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 144 144">${body}</svg>`;

write("save-key", saveKey("30s", "SAVE CLIP", C.red));
write("record-key", card({ accent: C.red, label: "REC", body: dot(C.red) }));
write("open-key", card({ accent: C.red, label: "CLIPS", body: film(C.red) }));
write("save", small(history("#ffffff", 72, 72, 44).replace(/stroke-width="3.2"/, `stroke-width="10"`)));
write("record", small(dot("#ffffff", 72, 72).replace(`r="26"`, `r="50"`).replace(`r="15"`, `r="28"`).replace(`stroke-width="4"`, `stroke-width="9"`)));
write("open", small(`<g transform="translate(72 72) scale(2) translate(-72 -52)">${film("#ffffff")}</g>`));
write("category", small(dot("#ffffff", 72, 72).replace(`r="26"`, `r="50"`).replace(`r="15"`, `r="28"`).replace(`stroke-width="4"`, `stroke-width="9"`)));
const EDGE = path.join(process.env["ProgramFiles(x86)"] ?? "C:/Program Files (x86)", "Microsoft/Edge/Application/msedge.exe");
const png = (name, svg, px) => {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clipback-imgs-"));
    const page = path.join(tmp, "page.html");
    // the page draws the SVG on a canvas and puts the PNG's base64 in the DOM, which --dump-dom prints
    const src = "data:image/svg+xml;base64," + Buffer.from(svg).toString("base64");
    fs.writeFileSync(page, `<!doctype html><pre id=out></pre><script>
const img = new Image();
img.onload = () => { const c = document.createElement("canvas"); c.width = c.height = ${px};
    c.getContext("2d").drawImage(img, 0, 0, ${px}, ${px}); out.textContent = "PNG:" + c.toDataURL("image/png").split(",")[1] + ":PNG"; };
img.src = "${src}";
</script>`);
    const dom = execFileSync(EDGE, ["--headless", "--disable-gpu", `--user-data-dir=${path.join(tmp, "profile")}`,
        "--virtual-time-budget=5000", "--dump-dom", "file:///" + page.replaceAll("\\", "/")],
        { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"], maxBuffer: 64 << 20 });
    fs.rmSync(tmp, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
    const b64 = /PNG:([A-Za-z0-9+/=]+):PNG/.exec(dom)?.[1];
    if (!b64) throw new Error(`Edge drew no ${name}.png`);
    fs.writeFileSync(path.join(dir, name + ".png"), Buffer.from(b64, "base64"));
};
const plugin = card({ accent: C.red, glow: 0.5, label: "clipback", body: dot(C.red) });
png("plugin", plugin, 256);
png("plugin@2x", plugin, 512);
