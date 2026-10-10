#!/usr/bin/env python3
"""LPK2 compiler/decoder using only the Python standard library."""
import argparse, hashlib, json, struct, sys
from pathlib import Path

def canonical(v):
    return json.dumps(v, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()
def sha(b): return hashlib.sha256(b).hexdigest()
def compile_lpk(project_path,out_path):
    p=json.loads(Path(project_path).read_text(encoding='utf-8'))
    if p.get('version')!=2: raise ValueError('project version must be 2')
    assets=[]; blobs=[]
    root=Path(project_path).parent
    for a in sorted((x for x in p['assets'] if x.get('role')=='raster'), key=lambda x:x['id']):
        raw=(root/a['path']).read_bytes()
        # Browser conversion produces RGB565LE + alpha8. Existing samples are already normalized.
        if len(raw)!=a['width']*a['height']*3:
            raise ValueError(f"asset {a['id']} is not normalized RGB565A8; use the browser compiler")
        assets.append({'id':a['id'],'width':a['width'],'height':a['height'],'offset':0,'length':len(raw),'sha256':sha(raw)})
        blobs.append(raw)
    scenes=[]
    for s in p['scenes']:
        variants={}
        for lid,v in s['variants'].items():
            layers=[]
            for l in v['layers']:
                x=dict(l)
                if x.get('kind')=='text':x['kind']='sprite';x['assetId']=x.pop('rasterAssetId')
                elif x.get('kind')=='image':x['kind']='sprite'
                layers.append(x)
            variants[lid]={'layers':layers}
        scenes.append({**s,'variants':variants})
    m={'version':2,'runtimeMinor':0,'renderer':'lp-raster-1','fps':30,'projectId':p['projectId'],'layouts':p['layouts'],'defaultLayout':p['defaultLayout'],'assets':assets,'scenes':scenes,'cues':p['cues']}
    offset=0
    for a in assets:a['offset']=offset;offset+=a['length']
    blob=b''.join(blobs); manifest=canonical(m); body=manifest+blob
    if len(manifest)>131072 or len(body)+64>2621440:raise ValueError('package too large')
    header=struct.pack('<4sHHIIII32sII',b'LPK2',2,0,64,len(manifest),len(blob),len(body)+64,hashlib.sha256(body).digest(),0,0)
    Path(out_path).write_bytes(header+body)
def decode(path):
    b=Path(path).read_bytes();
    if len(b)<64 or b[:4]!=b'LPK2':raise ValueError('invalid magic')
    _,major,minor,hs,ml,bl,total,digest,flags,res=struct.unpack('<4sHHIIII32sII',b[:64])
    if (major,hs,total,len(b))!=(2,64,64+ml+bl,len(b)):raise ValueError('invalid header')
    if hashlib.sha256(b[64:]).digest()!=digest:raise ValueError('digest mismatch')
    m=json.loads(b[64:64+ml]);pos=0
    for a in m['assets']:
        if a['offset']!=pos or a['length']!=a['width']*a['height']*3:raise ValueError('asset bounds')
        raw=b[64+ml+pos:64+ml+pos+a['length']]
        if sha(raw)!=a['sha256']:raise ValueError('asset hash')
        pos+=a['length']
    if pos!=bl:raise ValueError('blob bounds')
    return m,sha(b)
if __name__=='__main__':
    ap=argparse.ArgumentParser();sub=ap.add_subparsers(dest='cmd',required=True);c=sub.add_parser('compile');c.add_argument('project');c.add_argument('output');d=sub.add_parser('decode');d.add_argument('package');a=ap.parse_args()
    if a.cmd=='compile':compile_lpk(a.project,a.output);print(a.output)
    else:m,h=decode(a.package);print(json.dumps({'packageHash':h,'manifest':m},ensure_ascii=False,indent=2))
