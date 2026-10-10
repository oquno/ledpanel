idf_component_register(
  SRCS "main.cpp" "lpk_decoder.cpp" "rig_store.cpp" "node_state.cpp" "web_api.cpp" "hub75_hal.cpp" "config_store.cpp" "usb_setup.cpp" "runtime_package.cpp" "render_runtime.cpp" "content_store.cpp" "sync_protocol.cpp" "sync_runtime.cpp" "node_http_auth.cpp" "node_distribution.cpp" "topology.cpp"
  INCLUDE_DIRS "."
  REQUIRES esp_http_server esp_http_client esp_wifi esp_event esp_netif lwip nvs_flash app_update esp_partition esp_timer json driver freertos mbedtls esp_driver_gpio esp_driver_usb_serial_jtag efuse esp_psram hub75
)

set(LP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(LP_EDITOR "${LP_ROOT}/apps/editor")
set(LP_FORMATS "${LP_ROOT}/packages/formats")
set(LP_RENDERER "${LP_ROOT}/packages/render-core")
set(LP_WWW "${CMAKE_CURRENT_LIST_DIR}/../www")
function(lp_embed source stem)
  set(copy_path "${CMAKE_CURRENT_BINARY_DIR}/lp_${stem}.txt")
  configure_file("${source}" "${copy_path}" COPYONLY)
  target_add_binary_data(${COMPONENT_LIB} "${copy_path}" TEXT)
endfunction()

lp_embed("${LP_WWW}/index.html" login_html)
lp_embed("${LP_WWW}/login.mjs" login_module)
lp_embed("${LP_WWW}/portal.css" portal_style)
lp_embed("${LP_EDITOR}/index.html" editor_html)
lp_embed("${LP_EDITOR}/app.js" editor_app)
lp_embed("${LP_EDITOR}/style.css" editor_style)
lp_embed("${LP_EDITOR}/animation.mjs" editor_animation)
lp_embed("${LP_EDITOR}/deployment.mjs" editor_deployment)
lp_embed("${LP_EDITOR}/media.mjs" editor_media)
lp_embed("${LP_FORMATS}/index.mjs" formats_index)
lp_embed("${LP_FORMATS}/browser.mjs" formats_browser)
lp_embed("${LP_FORMATS}/crypto.mjs" formats_crypto)
lp_embed("${LP_FORMATS}/zip.mjs" formats_zip)
lp_embed("${LP_RENDERER}/index.mjs" renderer_index)
