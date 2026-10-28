/* Shared rasterization for platform and application icons. */
const sharp = require('sharp');

function inkBounds(data, width, height, threshold) {
    let left = width, top = height, right = -1, bottom = -1;
    for (let y = 0; y < height; ++y) for (let x = 0; x < width; ++x) {
        if (data[(y * width + x) * 4 + 3] < threshold) continue;
        left = Math.min(left, x); top = Math.min(top, y);
        right = Math.max(right, x); bottom = Math.max(bottom, y);
    }
    return right < left ? null : {left, top, width: right - left + 1, height: bottom - top + 1};
}

async function renderIcon(source, size, visibleHeight) {
    const {data, info} = await sharp(source, {density: 384})
        .resize(size, size, {fit: 'contain', background: {r: 0, g: 0, b: 0, alpha: 0}})
        .toColourspace('srgb').ensureAlpha().raw().toBuffer({resolveWithObject: true});
    if (info.channels !== 4 || info.width !== size || info.height !== size)
        throw new Error(`Unexpected icon format: ${source}`);
    for (let i = 0; i < data.length; i += 4) {
        data[i] = data[i + 1] = data[i + 2] = 0;
        data[i + 3] = Math.round(data[i + 3] / 17) * 17;
    }
    if (visibleHeight) {
        const bounds = inkBounds(data, size, size, 1);
        if (!bounds) throw new Error(`Empty status icon: ${source}`);
        /* Calibrate after resampling, against the half-coverage stroke edge. */
        for (let height = visibleHeight; height <= visibleHeight + 8; ++height) {
            const sample = await sharp(data, {raw: {width: size, height: size, channels: 4}})
                .extract(bounds).resize({height}).raw().toBuffer({resolveWithObject: true});
            for (let i = 3; i < sample.data.length; i += 4)
                sample.data[i] = Math.round(sample.data[i] / 17) * 17;
            const ink = inkBounds(sample.data, sample.info.width, sample.info.height, 128);
            if (ink && ink.height === visibleHeight)
                return sharp(sample.data, {raw: sample.info}).extract(ink).png().toBuffer();
        }
        throw new Error(`Cannot calibrate ${source} to ${visibleHeight}px`);
    }
    return sharp(data, {raw: {width: size, height: size, channels: 4}}).png().toBuffer();
}

module.exports = {renderIcon};
