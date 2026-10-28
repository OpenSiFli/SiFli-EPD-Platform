/* Render each application's own icon.svg using its local source metadata. */
const fs = require('node:fs');
const path = require('node:path');
const {renderIcon} = require('./icon_raster.cjs');

async function main(args = process.argv.slice(2)) {
    const modules = path.resolve(__dirname, '../modules');
    const available = fs.readdirSync(modules, {withFileTypes: true})
        .filter(entry => entry.isDirectory() &&
            fs.existsSync(path.join(modules, entry.name, 'app.json')) &&
            fs.existsSync(path.join(modules, entry.name, 'assets/icon.svg')))
        .map(entry => entry.name).sort();
    if (args.length && (args.length !== 2 || args[0] !== '--apps' || !args[1]))
        throw new Error('Usage: node tools/generate_app_icons.cjs [--apps id1,id2]');
    const selected = args.length ? [...new Set(args[1].split(','))] : available;
    for (const id of selected)
        if (!available.includes(id)) throw new Error(`Application icon not found: ${id}`);
    for (const id of selected) {
        const assets = path.join(modules, id, 'assets');
        const metadata = JSON.parse(fs.readFileSync(path.join(assets, 'icon.source.json'), 'utf8'));
        if (!Number.isInteger(metadata.size) || metadata.size <= 0)
            throw new Error(`Invalid icon size: ${id}`);
        fs.writeFileSync(path.join(assets, 'icon.png'),
            await renderIcon(path.join(assets, 'icon.svg'), metadata.size));
    }
    console.log(`Prepared ${selected.length} application icons`);
}

if (require.main === module) main().catch(error => {console.error(error); process.exitCode = 1;});
