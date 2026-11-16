// Writes the plugin's static images (imgs\) from the same drawing code the keys use:
//   node streamdeck\make-imgs.mjs
// *-key.svg is what a key shows before the plugin has drawn it; the others are the white icons in Stream Deck's
// action list, and the plugin's own icon.
import fs from "node:fs";
import path from "node:path";
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
write("plugin", card({ accent: C.red, glow: 0.5, label: "clipback", body: dot(C.red) }));
