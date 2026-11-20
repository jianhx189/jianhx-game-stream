const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const dist = path.join(root, 'dist');
const siteFiles = [
  'index.html',
  'en.html',
  'tutorial.html',
  'connect.html',
  'connect.js',
  'changelog.html',
  'styles.css',
  'script.js',
  'release.json',
  'updates.json',
  'robots.txt',
  'sitemap.xml'
];
const assetPattern = /\.(?:svg|jpg|jpeg|png|webp|gif|ico|woff2?)$/i;

fs.rmSync(dist, { recursive: true, force: true });
fs.mkdirSync(path.join(dist, 'assets'), { recursive: true });

for (const file of siteFiles) {
  const source = path.join(root, file);
  if (!fs.statSync(source).isFile()) throw new Error(`Site source is not a file: ${file}`);
  fs.copyFileSync(source, path.join(dist, file));
}

for (const entry of fs.readdirSync(path.join(root, 'assets'), { withFileTypes: true })) {
  if (entry.isFile() && assetPattern.test(entry.name)) {
    fs.copyFileSync(path.join(root, 'assets', entry.name), path.join(dist, 'assets', entry.name));
  }
}

console.log(`Static site ready in web/dist: ${siteFiles.length} site files plus allowlisted public assets.`);
