#!/usr/bin/env python3
import json, struct, hashlib
from pathlib import Path
root=Path(__file__).resolve().parents[1]
def sha(b):return hashlib.sha256(b).hexdigest()
for name in ('minimal','demo'):
    b=(root/'examples'/f'{name}.lpk').read_bytes();magic,major,minor,hs,ml,bl,total,digest,flags,res=struct.unpack('<4sHHIIII32sII',b[:64])
    assert (magic,major,minor,hs,total)==(b'LPK2',2,0,64,len(b));assert hashlib.sha256(b[64:]).digest()==digest
    m=json.loads(b[64:64+ml]);pos=0
    for a in m['assets']:
        assert a['offset']==pos and a['length']==a['width']*a['height']*3
        raw=b[64+ml+pos:64+ml+pos+a['length']];assert sha(raw)==a['sha256'];pos+=a['length']
    assert pos==bl
print('examples: LPK2 headers, hashes and asset bounds OK')
