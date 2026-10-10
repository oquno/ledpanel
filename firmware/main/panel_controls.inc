// Included inside web_api.cpp's private namespace: share the fleet command gate.
std::string display_preference(const PlaybackState& state) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "version", 1);
  cJSON_AddStringToObject(root, "hash", state.packageHash.c_str());
  cJSON_AddStringToObject(root, "layout", state.layoutId.c_str());
  cJSON_AddStringToObject(root, "mode", state.mode.c_str());
  cJSON_AddStringToObject(root, "scene", state.sceneId.c_str());
  cJSON_AddStringToObject(root, "cue", state.cueId.c_str());
  cJSON_AddBoolToObject(root, "running", state.running);
  cJSON_AddBoolToObject(root, "blackout", state.blackout);
  cJSON_AddNumberToObject(root, "brightness", state.userBrightness);
  char* text = cJSON_PrintUnformatted(root);
  std::string result = text ? text : "";
  cJSON_free(text); cJSON_Delete(root);
  return result;
}
std::string read_display_preference() {
  nvs_handle_t handle;
  if (nvs_open("display", NVS_READONLY, &handle) != ESP_OK) return "";
  size_t size = 0; std::string result;
  if (nvs_get_str(handle, "last", nullptr, &size) == ESP_OK && size > 1 && size <= 1024) {
    result.resize(size);
    if (nvs_get_str(handle, "last", result.data(), &size) == ESP_OK) result.resize(size-1);
    else result.clear();
  }
  nvs_close(handle); return result;
}
bool save_display_preference(const std::string& value) {
  if (value.empty()) return false;
  nvs_handle_t handle;
  if (nvs_open("display", NVS_READWRITE, &handle) != ESP_OK) return false;
  const bool saved = nvs_set_str(handle, "last", value.c_str()) == ESP_OK && nvs_commit(handle) == ESP_OK;
  nvs_close(handle); return saved;
}
// Clocks/revisions are deliberately never restored from a previous boot.
bool restore_display_preference(const std::string& value, PlaybackState& state) {
  cJSON* root = cJSON_Parse(value.c_str());
  const char* hash = json_string(root, "hash");
  const char* layout = json_string(root, "layout");
  const char* mode = json_string(root, "mode");
  const cJSON* running = root ? cJSON_GetObjectItemCaseSensitive(root,"running") : nullptr;
  const cJSON* blackout = root ? cJSON_GetObjectItemCaseSensitive(root,"blackout") : nullptr;
  uint32_t brightness = 0, version = 0;
  const bool valid = root && hash && layout && mode && state.packageHash == hash &&
      state.layoutId == layout && cJSON_IsBool(running) && cJSON_IsBool(blackout) &&
      json_uint32(cJSON_GetObjectItemCaseSensitive(root,"version"),version) && version == 1 &&
      json_uint32(cJSON_GetObjectItemCaseSensitive(root,"brightness"),brightness) && brightness <= 255 &&
      (std::strcmp(mode,"cue")==0 || std::strcmp(mode,"scene")==0 || std::strcmp(mode,"stopped")==0);
  if (valid) {
    state.mode = mode; state.running = cJSON_IsTrue(running) && state.mode != "stopped";
    state.blackout = cJSON_IsTrue(blackout); state.userBrightness = std::min<uint32_t>(brightness,g_config->brightnessCap);
    const char* scene = json_string(root,"scene"), *cue = json_string(root,"cue");
    state.sceneId = scene ? scene : ""; state.cueId = cue ? cue : "";
    state.positionTicks = 0; state.anchorUs = 0; state.effectiveAtUs = 0;
  }
  cJSON_Delete(root); return valid;
}

bool apply_panel_control(const char* op, const cJSON* args, std::string& error) {
  if (g_maintenance_state.load() != 0 || hub75_diagnostic_active() || sync_runtime_transaction_active()) return false;
  bool expected = false;
  if (!g_topology_sync_busy.compare_exchange_strong(expected,true)) return false;
  AtomicFlagReset reset(g_topology_sync_busy);
  PlaybackState current, next;
  if (!sync_runtime_state_snapshot(current) || !ensure_active_renderer(current.packageHash,error) ||
      !render_runtime_command(op,args,current,esp_timer_get_time(),next,error)) return false;
  next.userBrightness = std::min(next.userBrightness,g_config->brightnessCap);
  if (current.revision == UINT32_MAX) return false;
  next.revision = current.revision+1;
  if (required_follower_registered(next.layoutId)) {
    const uint64_t now = esp_timer_get_time(), boundary = now+1500000;
    if (current.running && std::strcmp(op,"next_scene")!=0 && std::strcmp(op,"go")!=0)
      next.positionTicks += 45;
    next.anchorUs = boundary; next.effectiveAtUs = boundary;
    if (!sync_runtime_apply_state(next,new_operation_id(),5000,error)) return false;
  } else {
    *g_state = next;
    if (!sync_runtime_update_state(next)) return false;
  }
  g_revision.fetch_add(1); return true;
}

bool restore_fleet_display(const std::string& preference, std::string& error) {
  if (g_maintenance_state.load()!=0 || sync_runtime_transaction_active()) return false;
  bool expected = false;
  if (!g_topology_sync_busy.compare_exchange_strong(expected,true)) return false;
  AtomicFlagReset reset(g_topology_sync_busy);
  ContentState content;
  TopologySnapshot topology;
  if (!content_get_state(content,error) || !content.has_active ||
      !build_current_topology(topology,error) || !ensure_active_renderer(content.active_hash,error)) return false;
  std::vector<RigFollowerIdentity> followers;
  std::vector<SyncPeerSnapshot> peers;
  if (!rig_display_followers(*g_config,topology.layout_id,followers,error)) return false;
  sync_runtime_peer_snapshot(peers);
  uint32_t revision = g_state->revision;
  for (const auto& follower:followers) {
    const auto peer = std::find_if(peers.begin(),peers.end(),[&](const SyncPeerSnapshot& item){
      return item.node_id==follower.node_id && item.hardware_id==follower.hardware_id && item.online && item.authenticated && item.clock_synced;
    });
    if (peer==peers.end()) { error="WAITING_FOR_PANELS"; return false; }
    if (peer->active_hash!=content.active_hash) { error="PANEL_CONTENT_MISMATCH"; return false; }
    revision=std::max(revision,peer->revision);
  }
  if (revision>=UINT32_MAX-2) {error="REVISION_EXHAUSTED";return false;}
  // Stop and reconstruct each follower via authenticated node HTTP, including
  // a follower that was still playing when only the parent was power-cycled.
  for (const auto& follower:followers) {
    if (!set_remote_maintenance(follower.node_id,true,error) ||
        !push_topology_to_follower(follower.node_id,topology,error)) return false;
    NodeContentMetadata metadata;
    if (!get_remote_content_metadata(follower,metadata,error)) return false;
    const uint32_t generation=std::max(content.generation,metadata.generation);
    if (generation>content.generation) {
      if (!content_advance_active_generation(content.active_hash,generation,error)) return false;
      content.generation=generation;
    }
    if (!activate_remote_candidate(follower.node_id,content.active_hash,generation,revision+1,error) ||
        !set_remote_maintenance(follower.node_id,false,error)) return false;
  }
  if (!apply_topology_snapshot(topology,error)) return false;
  g_topology_snapshot=topology;g_topology_snapshot_valid=true;g_topology_dirty.store(false);
  PlaybackState baseline=*g_state;
  baseline.revision=revision+1;baseline.packageHash=content.active_hash;
  baseline.running=false;baseline.mode="stopped";baseline.blackout=true;baseline.userBrightness=1;
  *g_state=baseline;
  if (!sync_runtime_update_state(baseline)) return false;
  // Wait for signed heartbeats to acknowledge the reconstructed baseline.
  vTaskDelay(pdMS_TO_TICKS(1200));
  PlaybackState desired=baseline;
  if (!restore_display_preference(preference,desired)) {
    cJSON* args=cJSON_CreateObject();
    const bool ok=render_runtime_command("next_scene",args,baseline,esp_timer_get_time(),desired,error);
    cJSON_Delete(args);
    if (!ok) return false;
    desired.blackout=false;desired.userBrightness=1;
  }
  // Validate saved scene/cue IDs against the loaded package before lighting.
  if (desired.mode=="scene" || desired.mode=="cue") {
    cJSON* args=cJSON_CreateObject();
    cJSON_AddStringToObject(args,desired.mode=="scene"?"sceneId":"cueId",
        desired.mode=="scene"?desired.sceneId.c_str():desired.cueId.c_str());
    PlaybackState validated;
    const bool ok=render_runtime_command(desired.mode=="scene"?"play_scene":"go",args,
        desired,esp_timer_get_time(),validated,error);
    cJSON_Delete(args);
    if (!ok) return false;
    validated.running=desired.running;desired=validated;
  }
  desired.revision=baseline.revision+1;
  desired.anchorUs=esp_timer_get_time()+1500000;desired.effectiveAtUs=desired.anchorUs;
  if (!followers.empty()) {
    if (!sync_runtime_apply_state(desired,new_operation_id(),5000,error)) return false;
  } else {
    *g_state=desired;
    if (!sync_runtime_update_state(desired)) return false;
  }
  g_revision.fetch_add(1);
  ESP_LOGI(kTag,"last display restored: %s, brightness=%u, blackout=%d",desired.layoutId.c_str(),desired.userBrightness,desired.blackout);
  return true;
}

uint8_t stepped_brightness(uint8_t value, bool brighter, uint8_t cap) {
  const uint8_t steps[]={1,2,4,8,16,26,40,64,96,128,192,255};
  if (brighter) {for (const uint8_t step:steps) if (step>value) return std::min(step,cap);return cap;}
  for (int i=sizeof(steps)-1;i>=0;--i) if (steps[i]<value) return steps[i];
  return std::min<uint8_t>(1,value);
}

bool display_controls_self_test() {
  PlaybackState source;
  source.packageHash=std::string(64,'a');source.layoutId="vertical";source.mode="scene";
  source.sceneId="test_scene";source.running=true;source.blackout=false;source.userBrightness=1;
  const std::string saved=display_preference(source);
  PlaybackState restored=source;restored.anchorUs=123;restored.positionTicks=999;
  if (!restore_display_preference(saved,restored) || restored.anchorUs!=0 || restored.positionTicks!=0 ||
      restored.userBrightness!=std::min<uint8_t>(1,g_config->brightnessCap) || !restored.running || restored.blackout || restored.sceneId!=source.sceneId) return false;
  restored.packageHash=std::string(64,'b');
  if (restore_display_preference(saved,restored) || restore_display_preference("{}",source)) return false;
  return stepped_brightness(0,false,64)==0 && stepped_brightness(0,true,64)==1 && stepped_brightness(1,false,64)==1 && stepped_brightness(1,true,64)==2 &&
      stepped_brightness(64,true,64)==64 && panel_keys_self_test();
}

void panel_controls_task(void*) {
  g_controls_self_test.store(display_controls_self_test());
  if (!g_controls_self_test.load()) {ESP_LOGE(kTag,"display control self-test failed");vTaskDelete(nullptr);return;}
  i2c_master_bus_handle_t bus=nullptr;i2c_master_dev_handle_t keys=nullptr;
  i2c_master_bus_config_t bus_config{};
  bus_config.i2c_port=I2C_NUM_0;bus_config.sda_io_num=GPIO_NUM_1;bus_config.scl_io_num=GPIO_NUM_2;
  bus_config.clk_source=I2C_CLK_SRC_DEFAULT;bus_config.glitch_ignore_cnt=7;
  bus_config.flags.enable_internal_pullup=true;
  if (i2c_new_master_bus(&bus_config,&bus)==ESP_OK) {
    i2c_device_config_t device{};device.dev_addr_length=I2C_ADDR_BIT_LEN_7;
    device.device_address=0x19;device.scl_speed_hz=100000;
    if (i2c_master_bus_add_device(bus,&device,&keys)==ESP_OK) {
      const auto read=[&](uint8_t reg,uint8_t& value){return i2c_master_transmit_receive(keys,&reg,1,&value,1,30)==ESP_OK;};
      uint8_t config=0,polarity=0;
      if (read(3,config) && read(2,polarity)) {
        // Preserve non-key pins (audio enables etc.) on the same expander.
        uint8_t command[]={3,static_cast<uint8_t>(config|14)};
        bool ok=i2c_master_transmit(keys,command,2,30)==ESP_OK;
        command[0]=2;command[1]=polarity&~14;
        ok=ok && i2c_master_transmit(keys,command,2,30)==ESP_OK;
        g_keys_available.store(ok);
      }
    }
  }
  ESP_LOGI(kTag,"panel switch %s",g_keys_available.load()?"ready (PCA9557)":"unavailable; browser controls remain usable");
  std::string saved=read_display_preference(),pending;
  uint64_t preference_changed=0,restore_at=esp_timer_get_time()+5000000;
  PanelKeys key_state;uint64_t health_check=0,mismatch_since=0,preference_check=0;
  for (;;) {
    if (!g_config->provisioned || g_config->role!=Role::Master) {vTaskDelay(pdMS_TO_TICKS(1000));continue;}
    const uint64_t now=esp_timer_get_time();
    if (g_restore_pending.load() && now>=restore_at) {
      std::string error;
      if (restore_fleet_display(saved,error)) g_restore_pending.store(false);
      else {restore_at=esp_timer_get_time()+3000000;ESP_LOGD(kTag,"display restore waiting: %s",error.c_str());}
    }
    if (!g_restore_pending.load() && now>=preference_check && g_maintenance_state.load()==0 && !g_topology_sync_busy.load() && !sync_runtime_transaction_active()) {
      preference_check=now+500000;
      PlaybackState state;
      if (sync_runtime_state_snapshot(state) && !state.packageHash.empty()) {
        const std::string value=display_preference(state);
        if (value!=pending) {pending=value;preference_changed=now;}
        if (value!=saved && now>=preference_changed+2000000 && save_display_preference(value)) saved=value;
      }
    }
    // Recover a panel powered on later, or a follower rebooted on its own.
    if (!g_restore_pending.load() && now>=health_check && g_maintenance_state.load()==0 &&
        !g_topology_sync_busy.load() && !sync_runtime_transaction_active()) {
      health_check=now+3000000;
      PlaybackState current;std::vector<RigFollowerIdentity> followers;
      std::vector<SyncPeerSnapshot> peers;std::string error;
      bool mismatch=false,all_online=true;
      if (sync_runtime_state_snapshot(current) && rig_display_followers(*g_config,current.layoutId,followers,error)) {
        sync_runtime_peer_snapshot(peers);
        for (const auto& follower:followers) {
          const auto peer=std::find_if(peers.begin(),peers.end(),[&](const SyncPeerSnapshot& item){return item.node_id==follower.node_id && item.hardware_id==follower.hardware_id;});
          if (peer==peers.end() || !peer->online || !peer->authenticated || !peer->clock_synced) {all_online=false;continue;}
          if (peer->active_hash==current.packageHash && (peer->revision!=current.revision || peer->topology_hash!=current.topologyHash)) mismatch=true;
        }
      }
      if (mismatch && all_online) {
        if (!mismatch_since) mismatch_since=now;
        else if (now>=mismatch_since+3000000) {g_restore_pending.store(true);restore_at=now;mismatch_since=0;}
      } else mismatch_since=0;
    }
    if (g_keys_available.load()) {
      uint8_t reg=0,value=14;
      if (i2c_master_transmit_receive(keys,&reg,1,&value,1,30)==ESP_OK) {
        value&=14;g_keys_raw.store(value);
        const auto event=key_state.update(value,now);
        if (event.darker || event.brighter || event.toggle || event.next) {
          std::string error;
          if (!g_restore_pending.load()) {
            if (event.next) apply_panel_control("next_scene",nullptr,error);
            if (event.toggle) {
              PlaybackState state;sync_runtime_state_snapshot(state);
              cJSON* args=cJSON_CreateObject();cJSON_AddBoolToObject(args,"enabled",!state.blackout);
              if (!state.running && state.blackout) {
                apply_panel_control("next_scene",nullptr,error);
                cJSON_ReplaceItemInObject(args,"enabled",cJSON_CreateBool(false));
              }
              apply_panel_control("blackout",args,error);cJSON_Delete(args);
            }
            if (event.darker || event.brighter) {
              PlaybackState state;sync_runtime_state_snapshot(state);
              cJSON* args=cJSON_CreateObject();
              cJSON_AddNumberToObject(args,"value",stepped_brightness(state.userBrightness,event.brighter,g_config->brightnessCap));
              apply_panel_control("brightness",args,error);cJSON_Delete(args);
            }
          }
          if (!error.empty()) ESP_LOGW(kTag,"panel switch action deferred: %s",error.c_str());
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
