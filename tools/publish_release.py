"""Publish an explicitly prepared release. Credentials stay in process memory.

Example: python tools/publish_release.py --root D:\\project --version 2.1.2
          --proxy http://127.0.0.1:7897
Hidden input JSON keys: github, cloudflare, account. Never save that JSON.
"""
import argparse, base64, getpass, hashlib, io, json, mimetypes, os
from pathlib import Path
import time, urllib.error, urllib.parse, urllib.request, uuid

parser=argparse.ArgumentParser()
parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1])
parser.add_argument('--version',required=True)
parser.add_argument('--proxy',default='')
parser.add_argument('--repo',default='jianhx189/jianhx-game-stream')
parser.add_argument('--cf-project',default='jianhx-game-stream')
args=parser.parse_args()
ROOT=args.root.resolve();TAG='v'+args.version
if args.proxy:os.environ['HTTPS_PROXY']=os.environ['HTTP_PROXY']=args.proxy
secret_json=os.environ.pop('MINISTREAM_RELEASE_CREDENTIALS',None)
if secret_json is None:
    secrets=json.loads(getpass.getpass('Release credentials JSON (hidden): '))
else:
    secrets=json.loads(secret_json)
gh=secrets.pop('github');cf_token=secrets.pop('cloudflare');account=secrets.pop('account','');del secrets,secret_json
os.environ.pop('GITHUB_TOKEN',None);os.environ.pop('CLOUDFLARE_API_TOKEN',None)

def request(url,method='GET',data=None,token=gh,ctype='application/json',missing=False):
    raw=data if isinstance(data,bytes) else (json.dumps(data,ensure_ascii=False).encode() if data is not None else None)
    binary=ctype=='application/octet-stream'
    headers={'Authorization':'Bearer '+token,'User-Agent':'MiniStream-Release','Accept':'application/vnd.github+json','Content-Type':ctype,'X-GitHub-Api-Version':'2022-11-28'}
    if raw is not None:headers['Content-Length']=str(len(raw))
    req=urllib.request.Request(url,data=io.BytesIO(raw) if binary else raw,method=method,headers=headers)
    try:
        with urllib.request.urlopen(req,timeout=240) as response:
            content=response.read();return json.loads(content) if content else None
    except urllib.error.HTTPError as exc:
        if missing and exc.code==404:return None
        # Do not echo request headers, credentials, response payloads or signed URLs.
        raise RuntimeError(f'{method} {urllib.parse.urlsplit(url).netloc} HTTP {exc.code}') from None

PREFIX='https://api.github.com/repos/'+args.repo
def api(path='',method='GET',data=None,**kw):return request(PREFIX+path,method,data,**kw)
def cf(path,method='GET',data=None,token=None,ctype='application/json'):
    response=request('https://api.cloudflare.com/client/v4'+path,method,data,token or cf_token,ctype)
    if not response.get('success'):raise RuntimeError('Cloudflare operation did not report success')
    return response['result']

user=request('https://api.github.com/user');api()
if not account:
    accounts=cf('/accounts?per_page=50')
    if len(accounts)!=1:raise RuntimeError('Cloudflare token must identify exactly one account or provide account')
    account=accounts[0]['id']
project_path=f'/accounts/{account}/pages/projects/{args.cf_project}'
cf(project_path)
print('AUTHENTICATED',user['login'],'GitHub and Cloudflare',flush=True)

release_dir=ROOT/'安装包'/args.version
if not release_dir.exists():release_dir=ROOT/'release'/args.version
manifest=json.loads((release_dir/'release-manifest.json').read_text(encoding='utf-8'))
if manifest['version']!=args.version:raise RuntimeError('Prepared manifest version mismatch')
for item in manifest['files']:
    file=release_dir/item['name']
    if file.stat().st_size!=item['size'] or hashlib.sha256(file.read_bytes()).hexdigest()!=item['sha256']:
        raise RuntimeError('Local release file checksum mismatch: '+item['name'])

def gather():
    items={}
    for name in ('README.md','README_EN.md','LICENSE','CHANGELOG.md','SECURITY.md','CONTRIBUTING.md','THIRD_PARTY_NOTICES.md','manifest.json'):
        items[name]=(ROOT/name).read_bytes()
    allowed={'.md','.py','.yml','.yaml','.html','.css','.js','.svg','.ico','.jpg','.png','.xml','.txt','.json','.cpp','.h','.qml','.qrc','.pro','.manifest','.iss','.ps1'}
    for directory in ('docs','tools','.github','web'):
        for file in (ROOT/directory).rglob('*'):
            if file.is_file() and file.suffix in allowed and '__pycache__' not in file.parts:
                items[file.relative_to(ROOT).as_posix()]=file.read_bytes()
    source=ROOT/'源码' if (ROOT/'源码').exists() else ROOT/'src'
    for local,remote in (('moonlight-qt/app','src/moonlight-qt/app'),('mslink','src/mslink'),('host-console','src/host-console')):
        for file in (source/local).rglob('*'):
            if file.is_file() and file.suffix in allowed and not any(p in ('build','release','debug','__pycache__') for p in file.relative_to(source/local).parts):
                items[remote+'/'+file.relative_to(source/local).as_posix()]=file.read_bytes()
    installer=source/'安装包脚本' if (source/'安装包脚本').exists() else ROOT/'scripts/installer'
    for file in installer.glob('*.iss'):items['scripts/installer/'+file.name]=file.read_bytes()
    host=ROOT/'构建产物/host/_internal' if (ROOT/'构建产物/host/_internal').exists() else ROOT/'scripts/host'
    for file in host.glob('*.ps1'):items['scripts/host/'+file.name]=file.read_bytes()
    items['docs/release-manifest.json']=(release_dir/'release-manifest.json').read_bytes()
    return items

def commit_files(items,message):
    head=api('/git/ref/heads/main')['object']['sha']
    previous=api('/git/commits/'+head)
    old={x['path']:x for x in api('/git/trees/'+previous['tree']['sha']+'?recursive=1')['tree']}
    changes=[]
    for path,data in items.items():
        sha=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
        if old.get(path,{}).get('sha')==sha:continue
        blob=api('/git/blobs','POST',{'content':base64.b64encode(data).decode(),'encoding':'base64'})
        changes.append({'path':path,'mode':'100644','type':'blob','sha':blob['sha']})
    if not changes:return head
    tree=api('/git/trees','POST',{'base_tree':previous['tree']['sha'],'tree':changes})
    commit=api('/git/commits','POST',{'message':message,'tree':tree['sha'],'parents':[head]})
    api('/git/refs/heads/main','PATCH',{'sha':commit['sha'],'force':False})
    print('COMMITTED',len(changes),commit['sha'],flush=True)
    return commit['sha']

items=gather()
source_commit=commit_files({k:v for k,v in items.items() if not k.startswith('web/dist/')},'release: MiniStream '+args.version+' transport robustness and credential protection')
notes=(ROOT/'docs/RELEASE_NOTES.md').read_text(encoding='utf-8')
notes+='\n\n### SHA-256\n'+''.join(f"- `{x['name']}` ({x['size']} bytes): `{x['sha256']}`\n" for x in manifest['files'])
release=next((r for r in api('/releases?per_page=100') if r['tag_name']==TAG),None)
if not release:
    release=api('/releases','POST',{'tag_name':TAG,'target_commitish':source_commit,'name':'MiniStream '+args.version+' · 正式版 / Release','body':notes,'draft':True,'prerelease':False,'make_latest':'false'})
for name in [x['name'] for x in manifest['files']]+['SHA256SUMS.txt','release-manifest.json']:
    file=release_dir/name;data=file.read_bytes();digest='sha256:'+hashlib.sha256(data).hexdigest()
    old=next((x for x in api(f"/releases/{release['id']}/assets") if x['name']==name),None)
    if old and old.get('digest')==digest:
        print('ASSET_VERIFIED',name,flush=True);continue
    if old:
        if not release['draft']:raise RuntimeError('Refusing to overwrite a different published asset: '+name)
        api('/releases/assets/'+str(old['id']),'DELETE')
    upload=release['upload_url'].split('{')[0]+'?name='+urllib.parse.quote(name)
    asset=request(upload,'POST',data,ctype='application/octet-stream')
    if asset.get('digest')!=digest:raise RuntimeError('Remote asset digest mismatch: '+name)
    print('ASSET_UPLOADED_VERIFIED',name,flush=True)
release=api('/releases/'+str(release['id']),'PATCH',{'draft':False,'prerelease':False,'make_latest':'true','name':'MiniStream '+args.version+' · 正式版 / Release','body':notes})
api('','PATCH',{'description':'MiniStream · Open-source Windows game & desktop streaming with Sunshine/Moonlight. 1 TCP + 1 UDP, native apps and real telemetry. 开源游戏串流。','homepage':'https://ministream.jianhx.com/'})
api('/topics','PUT',{'names':['game-streaming','remote-gaming','remote-desktop','windows','sunshine','moonlight','qt','cpp','frp','self-hosted','low-latency','udp','tcp']})
print('REGULAR_RELEASE_PUBLISHED',release['html_url'],flush=True)

website_commit=commit_files({k:v for k,v in items.items() if k.startswith('web/dist/')},'site: publish '+args.version+' downloads, English introduction and guides')
jwt=cf(project_path+'/upload-token')['jwt'];mapping={};payload=[]
for file in (ROOT/'web/dist').rglob('*'):
    if not file.is_file():continue
    if file.suffix not in {'.html','.js','.css','.svg','.jpg','.png','.xml','.txt','.json'}:raise RuntimeError('Unexpected website input')
    content=file.read_bytes();key='/'+file.relative_to(ROOT/'web/dist').as_posix();digest=hashlib.sha256(content+file.suffix.encode()).hexdigest()[:32]
    mapping[key]=digest
    mime=mimetypes.guess_type(file.name)[0] or 'application/octet-stream'
    if file.suffix in {'.html','.css','.js','.txt','.json','.xml'}:mime+='; charset=utf-8'
    payload.append({'key':digest,'value':base64.b64encode(content).decode(),'base64':True,'metadata':{'contentType':mime}})
cf('/pages/assets/upload','POST',payload,jwt)
boundary='----MiniStream'+uuid.uuid4().hex
fields={'manifest':json.dumps(mapping),'branch':'main','commit_hash':website_commit,'commit_message':'MiniStream '+args.version+' regular release'}
body=b''.join((f'--{boundary}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n').encode() for k,v in fields.items())+f'--{boundary}--\r\n'.encode()
deployment=cf(project_path+'/deployments','POST',body,ctype='multipart/form-data; boundary='+boundary)
for _ in range(30):
    if deployment.get('latest_stage',{}).get('status') in ('success','failure','canceled'):break
    time.sleep(2);deployment=cf(project_path+'/deployments/'+deployment['id'])
result={'version':args.version,'source_commit':source_commit,'website_commit':website_commit,'release':release['html_url'],'cloudflare_deployment':deployment['id'],'cloudflare_stage':deployment.get('latest_stage'),'website':'https://ministream.jianhx.com/','github_pages':'https://jianhx189.github.io/jianhx-game-stream/'}
(ROOT/'构建产物/publication-result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
print('PUBLICATION_RESULT',json.dumps(result,ensure_ascii=False),flush=True)
if deployment.get('latest_stage',{}).get('status')!='success':raise RuntimeError('Cloudflare deployment did not finish successfully')
