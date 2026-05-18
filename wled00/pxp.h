#pragma once

#include <Arduino.h>

constexpr uint16_t PXP_DEFAULT_PORT = 47987;
constexpr uint16_t PXP_MAX_PACKET_SIZE = 1200;

#ifdef WLED_ENABLE_PXP

void pxpBeginUdp();
void pxpHandleNotifications();
void pxpReadConfig(JsonObject ifLive);
void pxpWriteConfig(JsonObject ifLive);
void pxpReadSettings(AsyncWebServerRequest* request);
void pxpAppendSettingsJS(Print& settingsScript);

#else

inline void pxpBeginUdp() {}
inline void pxpHandleNotifications() {}
inline void pxpReadConfig(JsonObject) {}
inline void pxpWriteConfig(JsonObject) {}
inline void pxpReadSettings(AsyncWebServerRequest*) {}
inline void pxpAppendSettingsJS(Print&) {}

#endif
