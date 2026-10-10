#include "lpk_decoder.hpp"
#include <cstring>
#include <mbedtls/sha256.h>
namespace ledpanel {
static uint16_t u16(const uint8_t* p){return uint16_t(p[0])|uint16_t(p[1])<<8;}
static uint32_t u32(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
static bool hash32(const uint8_t* p,size_t n,uint8_t out[32]){mbedtls_sha256_context c;mbedtls_sha256_init(&c);if(mbedtls_sha256_starts(&c,0)!=0)return false;mbedtls_sha256_update(&c,p,n);const int r=mbedtls_sha256_finish(&c,out);mbedtls_sha256_free(&c);return r==0;}
bool decode_lpk(const uint8_t* b,size_t n,LpkPackage& out,std::string& error){
 if(!b||n<64){error="package too short";return false;}if(std::memcmp(b,"LPK2",4)!=0){error="LPK2 magic required";return false;}
 out.header.major=u16(b+4);out.header.minor=u16(b+6);out.header.headerSize=u32(b+8);out.header.manifestLength=u32(b+12);out.header.blobLength=u32(b+16);out.header.totalLength=u32(b+20);std::memcpy(out.header.digest,b+24,32);
 if(out.header.major!=2||out.header.headerSize!=64||out.header.totalLength!=n||uint64_t(out.header.manifestLength)+out.header.blobLength+64!=n){error="invalid package lengths";return false;}
 if(n>kMaxPackageBytes||out.header.manifestLength>131072){error="package exceeds limit";return false;}
 uint8_t digest[32];if(!hash32(b+64,n-64,digest)||std::memcmp(digest,out.header.digest,32)!=0){error="package digest mismatch";return false;}
 out.manifest.assign(reinterpret_cast<const char*>(b+64),out.header.manifestLength);out.blob=b+64+out.header.manifestLength;out.blobSize=out.header.blobLength;return true;
}
}
