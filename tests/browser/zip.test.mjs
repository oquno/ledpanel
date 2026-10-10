import test, { assert } from './harness.mjs';import {zipStore,unzipStore} from '/packages/formats/zip.mjs';
test('store ZIP roundtrip',()=>{const z=zipStore([{name:'project.json',data:'{}'},{name:'assets/x.bin',data:new Uint8Array([1,2,3])}]);const e=unzipStore(z);assert.equal(e.length,2);assert.equal(new TextDecoder().decode(e[0].data),'{}');assert.deepEqual([...e[1].data],[1,2,3])});
