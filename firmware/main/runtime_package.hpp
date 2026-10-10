#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace ledpanel {
bool validate_runtime_package(const uint8_t* bytes, size_t length,
                              std::string& package_hash, std::string& error);
bool runtime_package_get_layout(const uint8_t* bytes, size_t length,
                                const std::string& layout_id,
                                std::string& layout_json,
                                std::string& error);
bool runtime_package_get_default_layout(const uint8_t* bytes, size_t length,
                                        std::string& layout_id,
                                        std::string& error);
bool runtime_package_self_test(std::string& error);
}
