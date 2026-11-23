/* Start the shared UART owner after validating the explicit installation path.
 * Stock UART auto-installs complete applications; it cannot stage-only. */
static constexpr uint32_t screenBootstrapRetryInterval = 5000;
static constexpr uint8_t screenBootstrapMaxAttempts = 12;

static bool screenBootstrapSend() {
  DynamicJsonDocument request(512);
  request["client_token"] =
      "iotcloud-bootstrap-" + String(screenImage.bootstrap_token);
  JsonObject data = request.createNestedObject("data");
  data["upgrade_method"] = 1;
  JsonObject target = data.createNestedArray("upgrade_info").createNestedObject();
  target["device_type"] = "SENSOR3_MCU";
  target["to_version"] = "SV.99.00.01.00";
  return screenDelivery(screenRoot() + "/mqtt/service/ota", request, 0);
}

static void screenBootstrapPoll(uint32_t now) {
  if (!screenImage.active || !screenImage.bootstrap ||
      strcmp(screenImage.state, "waiting_for_mcu") ||
      screenImage.bootstrap_attempts >= screenBootstrapMaxAttempts ||
      (int32_t)(now - screenImage.bootstrap_retry_at) < 0 ||
      (int32_t)(now - screenImage.deadline) >= 0)
    return;
  screenImage.bootstrap_attempts++;
  screenImage.bootstrap_retry_at = now + screenBootstrapRetryInterval;
  /* The stock MCU ignores service messages until it has completed its
   * startup queries. Retransmit the same request while that window is open;
   * the MCU's maintenance state makes duplicate acceptance harmless. */
  screenBootstrapSend();
}

static const char *screenUpdateStart(JsonObjectConst params) {
  String mode = params["mode"] | "";
  bool bootstrap = mode == "factory-bootstrap";
  if ((mode != "stage" && !bootstrap) ||
      !screenFields(params, "|mode||sha256|") ||
      params.size() != (bootstrap ? 2u : 1u))
    return "invalid_params";
  if (screenUploadBusy || screenWallpaper.active || screenWallpaper.pending || screenFontBusy())
    return "busy";
  screenImportArtifact();
  if (!screenImage.valid)
    return "not_ready";
  if (screenImage.active)
    return "busy";
  if (bootstrap) {
    if (screenGuardAvailable() ||
        !params["sha256"].is<const char *>() ||
        String(params["sha256"].as<const char *>()) != screenHex(screenImage.sha, 32))
      return "invalid_firmware";
  } else if (!screenGuardAvailable())
    return "not_ready";
  if (!screenStoreMutex || xSemaphoreTake(screenStoreMutex, 0) != pdTRUE)
    return "busy";
  if (!screenImageVerify()) {
    xSemaphoreGive(screenStoreMutex);
    return "invalid_firmware";
  }
  portENTER_CRITICAL(&screenImageMux);
  screenImage.active = true;
  // Delayed business replies must not interleave with factory raw blocks.
  while (screenHead != screenTail) {
    screenQueue[screenHead].wire = "";
    screenHead = (screenHead + 1) % 12;
  }
  portEXIT_CRITICAL(&screenImageMux);
  xSemaphoreGive(screenStoreMutex);
  screenImage.pending = false;
  screenImage.bootstrap = bootstrap;
  screenImage.offset = 0;
  screenOtaError = "";
  uint32_t now = millis();
  screenImage.deadline = now + 60000;
  screenImage.state = "waiting_for_mcu";
  screenImage.bootstrap_token = now;
  screenImage.bootstrap_attempts = bootstrap ? 1 : 0;
  screenImage.bootstrap_retry_at =
      bootstrap ? now + screenBootstrapRetryInterval : 0;
  if (!screenMediaQuiesce()) {
    screenImage.active = false;
    screenImage.state = "uart_busy";
    return "busy";
  }
  bool sent;
  if (bootstrap)
    sent = screenBootstrapSend();
  else
    sent = screenSend("+screen_recover:1\r\n");
  if (!sent) {
    screenImage.active = false;
    return "busy";
  }
  return "accepted";
}
