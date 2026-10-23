// Development asset generator, not part of the C++ product runtime.
// Usage: NODE_PATH=<directory containing sharp> node assets/render-icon.cjs
const fs = require('node:fs');
const path = require('node:path');
const sharp = require('sharp');
(async () => {
  const folder = __dirname;
  const svg = fs.readFileSync(path.join(folder, 'app-icon.svg'));
  const sizes = [16, 20, 24, 32, 48, 64, 128, 256];
  const frames = [];
  for (const size of sizes) {
    // Render from the vector at every target size; no magnified low-res bitmap.
    const png = await sharp(svg, { density: 384 }).resize(size, size).png().toBuffer();
    frames.push(png);
    if (size === 256) fs.writeFileSync(path.join(folder, 'app-icon-256.png'), png);
  }
  const directory = Buffer.alloc(6 + sizes.length * 16);
  directory.writeUInt16LE(1, 2);
  directory.writeUInt16LE(sizes.length, 4);
  let offset = directory.length;
  for (let index = 0; index < sizes.length; ++index) {
    const entry = 6 + index * 16;
    directory[entry] = directory[entry + 1] = sizes[index] === 256 ? 0 : sizes[index];
    directory.writeUInt16LE(1, entry + 4);
    directory.writeUInt16LE(32, entry + 6);
    directory.writeUInt32LE(frames[index].length, entry + 8);
    directory.writeUInt32LE(offset, entry + 12);
    offset += frames[index].length;
  }
  fs.writeFileSync(path.join(folder, 'app.ico'), Buffer.concat([directory, ...frames]));
  console.log(`Created app.ico: ${sizes.join(', ')} px; transparent PNG frames.`);
})().catch(error => { console.error(error.message); process.exitCode = 1; });
