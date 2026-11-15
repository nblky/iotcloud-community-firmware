#pragma once
#include "../common/typed_wire.h"

enum { TC1_DIAG_WIFI_UP, TC1_DIAG_WIFI_DOWN, TC1_DIAG_SCAN_START,
       TC1_DIAG_SCAN_DONE, TC1_DIAG_NTP_START, TC1_DIAG_NTP_OK,
       TC1_DIAG_NTP_ERROR, TC1_DIAG_EVENTS };
void tc1_diag_event(unsigned event);
void tc1_heap_diag_mp(ph_writer *, uint32_t after);
void tc1_rx_diag_mp(ph_writer *, uint32_t after);
void tc1_net_diag_mp(ph_writer *);

void tc1_diag_alloc(void *,size_t,uint32_t);
void tc1_diag_free(void *);
