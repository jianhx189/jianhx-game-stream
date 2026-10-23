// Build a self-contained static distribution. No dependencies are installed.
const fs=require('fs'),path=require('path');
const root=path.resolve(__dirname,'..');
const design=path.resolve(root,'..','design','client-ui');
const assets=path.join(root,'assets');
fs.mkdirSync(assets,{recursive:true});
for(const file of ['brand.svg','wallpaper.svg'])fs.copyFileSync(path.join(design,'assets',file),path.join(assets,file));
for(const [source,target] of [['01-devices.png','client-devices.png'],['02-session.png','client-session.png'],['03-input-error.png','client-input-error.png'],['05-diagnostics.png','client-diagnostics.png']])fs.copyFileSync(path.join(design,'previews',source),path.join(assets,target));
const dist=path.join(root,'dist');
fs.mkdirSync(path.join(dist,'assets'),{recursive:true});
for(const file of ['index.html','styles.css','script.js','robots.txt','sitemap.xml'])fs.copyFileSync(path.join(root,file),path.join(dist,file));
for(const file of fs.readdirSync(assets))if(fs.statSync(path.join(assets,file)).isFile())fs.copyFileSync(path.join(assets,file),path.join(dist,'assets',file));
console.log('Static site prepared: '+dist);
