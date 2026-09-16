// Render native vector artwork, not an edit of the generated bitmap.
// node scripts/render_launcher_icons.cjs [/absolute/path/to/@resvg/resvg-js]
const { readFileSync, writeFileSync } = require('node:fs');
const { resolve } = require('node:path');
const { Resvg } = require(process.argv[2] || '@resvg/resvg-js');

const root = resolve(__dirname, '..');
const source = readFileSync(resolve(root, 'docs/branding/launcher.svg'), 'utf8');
const rectangle = '<rect x="18" y="18" width="72" height="72" rx="16"/>';
if (!source.includes(rectangle)) throw new Error('Expected launcher mask not found');
const round = source.replace(rectangle, '<circle cx="54" cy="54" r="36"/>');
const res = resolve(root, 'android/XiaoGpsTrackerApp/app/src/main/res');

function render(svg, size, destination) {
    const renderer = new Resvg(svg, {
        fitTo: { mode: 'width', value: size },
        font: { loadSystemFonts: false },
    });
    writeFileSync(destination, renderer.render().asPng());
}

for (const [density, size] of Object.entries({mdpi:48, hdpi:72, xhdpi:96, xxhdpi:144, xxxhdpi:192})) {
    render(source, size, resolve(res, `mipmap-${density}/ic_launcher.png`));
    render(round, size, resolve(res, `mipmap-${density}/ic_launcher_round.png`));
}
render(source, 512, resolve(root, 'docs/branding/launcher-preview.png'));
console.log('Rendered 10 launcher PNGs and the 512px preview.');
