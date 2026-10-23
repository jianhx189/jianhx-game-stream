const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const dist = path.join(root, 'dist');
fs.mkdirSync(path.join(dist, 'assets'), {recursive:true});
for (const file of ['index.html','tutorial.html','styles.css','script.js','robots.txt','sitemap.xml']) {
  fs.copyFileSync(path.join(root,file),path.join(dist,file));
}
for (const file of fs.readdirSync(path.join(root,'assets'))) {
  if (/\.(svg|jpg)$/.test(file)) fs.copyFileSync(path.join(root,'assets',file),path.join(dist,'assets',file));
}
console.log('Static site ready in web/dist');
