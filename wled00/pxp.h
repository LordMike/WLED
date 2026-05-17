#pragma once

#ifdef WLED_ENABLE_PXP

#include <Arduino.h>

constexpr uint16_t PXP_DEFAULT_PORT = 47987;
constexpr uint16_t PXP_MAX_PACKET_SIZE = 1200;

void pxpBeginUdp();
void pxpHandleScheduled();
void pxpHandle();

#endif
