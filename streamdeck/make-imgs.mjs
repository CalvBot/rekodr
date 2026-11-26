// Writes the plugin's static key images and action icons (imgs\) from the same drawing code the keys use:
//   node streamdeck\make-imgs.mjs
// *-key.svg is what a key shows before the plugin has drawn it; the others are the white icons in Stream Deck's
// action list. The plugin and category icons are PNGs drawn from the logo, on the app icon's plate.
import fs from "node:fs";
import path from "node:path";
import { C, card, saveKey, history, film, dot } from "./dev.calvbot.rekodr.sdPlugin/render.mjs";
const dir = path.join(import.meta.dirname, "dev.calvbot.rekodr.sdPlugin", "imgs");
fs.mkdirSync(dir, { recursive: true });
const write = (name, svg) => fs.writeFileSync(path.join(dir, name + ".svg"), svg + "\n");
const small = body => `<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 144 144">${body}</svg>`;

write("save-key", saveKey("30s", "SAVE CLIP", C.accent));
write("record-key", card({ accent: C.accent, label: "REC", body: dot(C.red) }));
write("open-key", card({ accent: C.accent, label: "CLIPS", body: film(C.accent) }));
write("save", small(history("#ffffff", 72, 72, 44).replace(/stroke-width="3.2"/, `stroke-width="10"`)));
write("record", small(dot("#ffffff", 72, 72).replace(`r="26"`, `r="50"`).replace(`r="15"`, `r="28"`).replace(`stroke-width="4"`, `stroke-width="9"`)));
write("open", small(`<g transform="translate(72 72) scale(2) translate(-72 -52)">${film("#ffffff")}</g>`));
