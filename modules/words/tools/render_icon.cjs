const path = require('node:path');
const fs = require('node:fs');
const {renderIcon} = require('../../../tools/icon_raster.cjs');
const icon = require('../assets/icon.source.json');

async function main() {
    const root = path.resolve(__dirname, '..');
    const output = path.join(root, 'output', 'assets');
    fs.mkdirSync(output, {recursive: true});
    const png = await renderIcon(path.join(root, 'assets', 'icon.svg'), icon.size);
    fs.writeFileSync(path.join(output, 'icon.png'), png);
}
main().catch(error => { console.error(error); process.exitCode = 1; });
