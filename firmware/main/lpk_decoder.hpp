#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
namespace ledpanel {
constexpr uint32_t kMaxPackageBytes = 2621440;
struct LpkHeader { uint16_t major, minor; uint32_t headerSize, manifestLength, blobLength, totalLength; uint8_t digest[32]; };
struct LpkPackage { LpkHeader header{}; std::string manifest; const uint8_t* blob=nullptr; size_t blobSize=0; };
bool decode_lpk(const uint8_t* bytes, size_t length, LpkPackage& out, std::string& error);
}
