#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ledpanel {

bool node_http_sign_request(const std::string& session_key_hex,
                            const std::string& method,
                            const std::string& path,
                            const std::string& group_id,
                            const std::string& sender_boot_id,
                            const std::string& receiver_boot_id,
                            uint32_t sequence,
                            const std::string& nonce_hex,
                            const uint8_t* body, size_t body_length,
                            std::string& body_hash_hex,
                            std::string& mac_hex, std::string& error);

bool node_http_verify_request(const std::string& session_key_hex,
                              const std::string& method,
                              const std::string& path,
                              const std::string& group_id,
                              const std::string& sender_boot_id,
                              const std::string& receiver_boot_id,
                              uint32_t sequence,
                              const std::string& nonce_hex,
                              const std::string& declared_body_hash_hex,
                              const std::string& declared_mac_hex,
                              const uint8_t* body, size_t body_length,
                              std::string& error);

bool node_http_sign_response(const std::string& session_key_hex,
                             uint16_t status_code,
                             const std::string& path,
                             uint32_t sequence,
                             const std::string& nonce_hex,
                             const uint8_t* body, size_t body_length,
                             std::string& body_hash_hex,
                             std::string& mac_hex, std::string& error);

bool node_http_verify_response(const std::string& session_key_hex,
                               uint16_t status_code,
                               const std::string& path,
                               uint32_t sequence,
                               const std::string& nonce_hex,
                               const std::string& declared_body_hash_hex,
                               const std::string& declared_mac_hex,
                               const uint8_t* body, size_t body_length,
                               std::string& error);

bool node_http_auth_self_test(std::string& error);

}  // namespace ledpanel
