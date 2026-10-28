/* Prepare the existing weather-code filenames from selected library SVGs. */
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {renderIcon} = require('../../../tools/icon_raster.cjs');
const manifest = require('../assets/weather/manifest.json');
const libraries = require('../assets/icons/manifest.json').libraries;
const root = path.resolve(__dirname, '..');

function moonPhaseSvg(source, phase) {
    if (!Number.isInteger(phase) || phase < 0 || phase > 7) throw new Error('Invalid moon phase');
    // Retain the pinned MingCute full_moon outer contour, omitting its crater marks.
    const outline = source.match(/M21 12C[^Z]+Z/);
    if (!outline) throw new Error('MingCute full_moon outer contour not found');
    const waxing = phase <= 4 ? phase : 8 - phase;
    let shadow = '';
    if (waxing === 0) shadow = `<path d="${outline[0]}" fill="black"/>`;
    else if (waxing !== 4) {
        // A sphere's terminator projects to a half ellipse: rx = r * |cos(theta)|.
        // Waxing illuminates the right; reflecting the shadow produces waning.
        const cosine = Math.cos(waxing * Math.PI / 4);
        const radiusX = (9 * Math.abs(cosine)).toFixed(6);
        const terminator = waxing === 2 ? 'L12 3' : `A${radiusX} 9 0 0 ${cosine < 0 ? 1 : 0} 12 3`;
        const reflect = phase > 4 ? ' transform="translate(24 0) scale(-1 1)"' : '';
        shadow = `<path d="M12 3A9 9 0 0 0 12 21${terminator}Z" fill="black"${reflect}/>`;
    }
    return `<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none">
<!-- Derived from MingCute full_moon.svg: outer contour retained, crater marks removed,
     projected-sphere phase shadow added. Attribution and formula: source.json. -->
${shadow}
<path d="${outline[0]}" stroke="black" stroke-width="2" stroke-linecap="round"/>
</svg>
`;
}

async function main() {
    const output = path.join(root, 'assets/ezip/weather');
    const indicators = path.join(root, 'assets/ezip/icons');
    const phases = path.join(root, 'assets/weather/phases');
    fs.mkdirSync(output, {recursive: true});
    fs.mkdirSync(phases, {recursive: true});
    fs.mkdirSync(indicators, {recursive: true});
    for (const icon of require('../assets/icons/manifest.json').icons)
        fs.writeFileSync(path.join(indicators, `ui_icon_${icon.name}.png`),
            await renderIcon(path.join(root, 'assets/icons', icon.source), icon.size));
    const codes = new Set();
    const rendered = new Map();
    const phaseSources = [];
    for (const group of manifest.groups) {
        for (const code of group.codes) {
            if (codes.has(code)) throw new Error(`Duplicate weather code: ${code}`);
            codes.add(code);
        }
        let source = path.join(root, 'assets/icons', group.source);
        if (group.phase !== undefined) {
            const original = fs.readFileSync(source);
            const svg = moonPhaseSvg(original.toString('utf8'), group.phase);
            for (const code of group.codes) fs.writeFileSync(path.join(phases, `w${code}.svg`), svg);
            phaseSources.push({codes: group.codes, phase: group.phase, condition: group.condition,
                source: group.source, source_sha256: crypto.createHash('sha256').update(original).digest('hex')});
            source = Buffer.from(svg);
        }
        for (const size of manifest.sizes) {
            const key = `${group.source}:${group.phase}:${size}`;
            if (!rendered.has(key)) rendered.set(key, await renderIcon(source, size));
            const png = rendered.get(key);
            for (const code of group.codes) {
                fs.writeFileSync(path.join(output, `w${code}_${size}.png`), png);
                if (size === 160) fs.writeFileSync(path.join(root, 'assets/weather', `w${code}.png`), png);
            }
        }
    }
    if (codes.size !== 70 || !codes.has(999)) throw new Error('Expected the 70 supported weather codes');
    fs.writeFileSync(path.join(phases, 'source.json'), JSON.stringify({
        schema_version: 1, library: 'mingcute', ...libraries.mingcute,
        license_file: 'modules/weather/assets/icons/LICENSE.MingCute',
        derivation: 'Retain full_moon outer contour; remove craters; add black unlit hemisphere using an elliptical terminator.',
        geometry: {center: [12, 12], radius: 9, outline_width: 2,
            phase_angle: 'theta = phase * pi / 4', lit_fraction: '(1 - cos(theta)) / 2',
            terminator_radius_x: '9 * abs(cos(theta))',
            orientation: 'Waxing is lit on the right; waning is its horizontal reflection.',
            light: 'transparent (white UI background)', shadow: 'black'},
        phases: phaseSources
    }, null, 2) + '\n');
    console.log(`Prepared ${codes.size} weather codes at ${manifest.sizes.join('/')} px`);
}
if (require.main === module) main().catch(error => {console.error(error); process.exitCode = 1;});
module.exports = {moonPhaseSvg};
