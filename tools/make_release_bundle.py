"""Freeze installers, current source and public manifests. Does not publish."""
import argparse, fnmatch, hashlib, json, re, shutil, subprocess, zipfile
from datetime import datetime, timezone
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('--version',default='2.1.2')
parser.add_argument('--moonlight',type=Path)
parser.add_argument('--forbid-text',action='append',default=[])
args=parser.parse_args()
ROOT=Path(__file__).resolve().parents[1]
TREE=(args.moonlight or ROOT/'构建环境/mlqt').resolve()
OUT=ROOT/'安装包'/args.version;OUT.mkdir(parents=True,exist_ok=True)
from sync_docs import sync
from update_site import sync_dist
sync()
bad_token=re.compile(rb'(?:github_pat_[A-Za-z0-9_]{25,}|cfat_[A-Za-z0-9]{20,})')
def check(data,name):
    if bad_token.search(data) or any(s.encode() in data for s in args.forbid_text):
        raise RuntimeError('Private marker in release input: '+name)
def sha(data):return hashlib.sha256(data).hexdigest()

# Mirror Inno exclusions and inspect the exact candidate file set before packaging.
inputs={}
for kind,folder in [('client',ROOT/'构建产物/client-preview'),('host',ROOT/'构建产物/host')]:
    selected=[]
    for file in folder.rglob('*'):
        if not file.is_file():continue
        rel=file.relative_to(folder).as_posix();low=rel.lower()
        excluded=any(fnmatch.fnmatch(low,p) for p in ('*.log','*.dmp','*.pdb','*.obj','mslink-status.json','mslink-status.jsonl','mslink-status.jsonl.previous'))
        excluded|=(kind=='client' and (low=='moonlight.exe' or low=='mslink.ini'))
        excluded|=(kind=='host' and (low=='mslink-host.ini' or low.startswith('sunshine/config/')))
        if excluded:continue
        if file.suffix.lower() in ('.pem','.dmp','.backup','.bak') or file.name in ('安装信息.txt','配置指南.html'):
            raise RuntimeError('Unexpected private/runtime file in package: '+rel)
        data=file.read_bytes();check(data,kind+'/'+rel)
        selected.append({'path':rel,'size':len(data),'sha256':sha(data)})
    inputs[kind]=selected

# Full tracked Moonlight source, including working changes and checked-out submodules.
def tracked(directory):
    names=subprocess.check_output(['git','-C',str(directory),'ls-files','-z']).decode('utf-8').split('\0')
    return [directory/n for n in names if n and (directory/n).is_file()]
full={p.relative_to(TREE).as_posix():p.read_bytes() for p in tracked(TREE)}
modules=subprocess.check_output(['git','-C',str(TREE),'submodule','status','--recursive']).decode().splitlines()
for line in modules:
    parts=line.strip().split()
    if len(parts)<2:continue
    sub=TREE/parts[1]
    for file in tracked(sub):full[file.relative_to(TREE).as_posix()]=file.read_bytes()
overlay=ROOT/'源码/moonlight-qt/app'
for file in overlay.rglob('*'):
    if file.is_file() and file.suffix in {'.cpp','.h','.qml','.qrc','.svg','.ico','.pro','.txt','.manifest'}:
        full['app/'+file.relative_to(overlay).as_posix()]=file.read_bytes()
source={}
for name,data in full.items():source['moonlight-qt/'+name]=data
for local,remote in [('源码/mslink','src/mslink'),('源码/host-console','src/host-console'),('源码/安装包脚本','scripts/installer'),('构建产物/host/_internal','scripts/host'),('docs','docs'),('tools','tools'),('web','web')]:
    base=ROOT/local
    for file in base.rglob('*'):
        if file.is_file() and file.suffix in {'.cpp','.h','.qml','.qrc','.svg','.ico','.pro','.txt','.md','.py','.iss','.ps1','.html','.css','.js','.jpg','.png','.xml','.json'} and not any(p in ('build','release','debug','__pycache__','dist') for p in file.relative_to(base).parts) and file.name not in ('release.json','release-manifest.json'):
            rel=file.relative_to(base).as_posix();data=file.read_bytes();source[remote+'/'+rel]=data
            if local=='源码/mslink':source['moonlight-qt/mslink/'+rel]=data
for name in ('README.md','README_EN.md','LICENSE','CHANGELOG.md','SECURITY.md','CONTRIBUTING.md','THIRD_PARTY_NOTICES.md'):
    source[name]=(ROOT/name).read_bytes()
refs={'moonlight_commit':'dc92b6d2f361064d3e9e53ce5de92f611db77220','windows_dependencies':'https://github.com/moonlight-stream/moonlight-qt-deps/releases/tag/v19','qt':'6.9.3','sunshine_tag':'v2026.914.233613','sunshine_commit':'63d35f702ee9e362e43263742981836ec0710384','sunshine_source':'https://github.com/LizardByte/Sunshine/archive/63d35f702ee9e362e43263742981836ec0710384.tar.gz','submodules':modules}
source['SOURCE_REFERENCES.json']=json.dumps(refs,ensure_ascii=False,indent=2).encode()
bundle=OUT/('MiniStream-'+args.version+'-Source.zip')
with zipfile.ZipFile(bundle,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
    for name,data in sorted(source.items()):
        check(data,'source/'+name)
        entry=zipfile.ZipInfo('MiniStream-'+args.version+'/'+name,date_time=(2026,10,23,0,0,0));entry.compress_type=zipfile.ZIP_DEFLATED;entry.external_attr=0o100644<<16
        archive.writestr(entry,data)
files=[]
for name in ('MiniStream-Client-Setup.exe','MiniStream-Host-Setup.exe'):
    shutil.copy2(ROOT/'构建产物/setup'/name,OUT/name)
for file in [OUT/'MiniStream-Client-Setup.exe',OUT/'MiniStream-Host-Setup.exe',bundle]:
    data=file.read_bytes();files.append({'name':file.name,'size':len(data),'sha256':sha(data)})
manifest={'product':'MiniStream','version':args.version,'channel':'stable','created_utc':datetime.now(timezone.utc).isoformat(),'files':files,'sources':refs,'compiler':'MSVC 14.44 x64 / Qt 6.9.3','privacy':'Private configuration, Sunshine config, logs, dumps and state files excluded; explicit package input scan passed.','verification':['CMake/CTest: 2/2','Local address source preservation: 60/60','UDP burst: 100/100','Authenticated forwarding probes recovered to UDP on affected multi-interface host'],'limits':['Clean Win10/Win11 installation and all driver combinations not comprehensively covered','Long-duration cross-network media/input testing remains ongoing','Custom tunnel/key exchange is not an independently audited encrypted VPN'],'package_input_files':inputs}
(OUT/'release-manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
checksum_files=files+[{'name':'release-manifest.json','sha256':sha((OUT/'release-manifest.json').read_bytes())}]
(OUT/'SHA256SUMS.txt').write_text(''.join(x['sha256']+'  '+x['name']+'\n' for x in checksum_files),encoding='utf-8')
release={'tag_name':'v'+args.version,'published_at':datetime.now(timezone.utc).isoformat(),'assets':[dict(x,browser_download_url='https://github.com/jianhx189/jianhx-game-stream/releases/download/v'+args.version+'/'+x['name']) for x in files]}
(ROOT/'web/release.json').write_text(json.dumps(release,ensure_ascii=False,indent=2),encoding='utf-8')
(ROOT/'web/updates.json').write_text(json.dumps([{'platform':'windows','arch':'x86_64','version':args.version,'browser_url':'https://github.com/jianhx189/jianhx-game-stream/releases/tag/v'+args.version}],indent=2),encoding='utf-8')
sync_dist()
print(json.dumps({'version':args.version,'assets':files,'source_files':len(source),'package_inputs':{k:len(v) for k,v in inputs.items()}},ensure_ascii=False,indent=2))
