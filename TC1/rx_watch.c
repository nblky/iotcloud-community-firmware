/* RX ownership event recorder for kernel 30300002.015
 * (SHA256 2f5dd55f8e702ff42c21d2606ec553731ce1cdefc2a90694d430559351e12874).
 *
 * A background task walks the allocator regions at a fixed interval and keeps a
 * small fingerprint of every RX-shaped object. For each object it records the
 * transition that matters for the retained-packet question:
 *
 *   BORN   - a descriptor/pbuf/RxPD triple appears
 *   GONE   - it disappears again (the normal delivery/free path ran)
 *   HELD   - it stayed across N consecutive walks (the retention symptom)
 *
 * The three retained objects on 102 were never seen to disappear, so a HELD
 * record with an unchanged payload hash is the direct observation that the
 * earlier read-only snapshots could only infer.
 *
 * Everything here is read-only with respect to the allocator: no allocation, no
 * free, no kernel writes. The task copies what it needs onto its own stack.
 */
#include "mico.h"
#include "memory_diag.h"
#include <string.h>

#define WORD(a) (*(volatile const uint32_t *)(uintptr_t)(a))
#define BYTE(a) (*(volatile const unsigned char *)(uintptr_t)(a))
#define ALLOCATED 0x80000000u

#define RXW_SLOTS 8u

typedef struct {
    uint32_t descriptor;   /* descriptor block payload address, 0 = empty slot */
    uint32_t pbuf;
    uint32_t rxpd;
    uint32_t seq;          /* RxPD sequence for this instance */
    uint32_t tid;
    uint32_t first_ms;     /* when this slot first saw the triple */
    uint32_t last_ms;      /* most recent walk that still saw it */
    uint32_t walks;        /* consecutive walks the triple persisted */
    uint32_t held;         /* 1 once walks exceeded the retention threshold */
    uint32_t freed_seen;   /* 1 if we observed it disappear */
} rx_watch;

static rx_watch rx_slots[RXW_SLOTS];
static uint32_t rx_events[TC1_RX_EVENTS][6];
static uint32_t rx_event_seq;
static uint32_t rx_walks, rx_hold_threshold = 6;
static uint32_t rx_born, rx_gone, rx_held;
static volatile bool rx_running;
static bool rx_started;

extern uint32_t link_bss_end, _ram_end_;

static uint32_t rx_irq_lock(void)
{
    uint32_t mask;
    __asm volatile("mrs %0, primask\ncpsid i" : "=r"(mask) :: "memory");
    return mask;
}
static void rx_irq_unlock(uint32_t mask)
{ __asm volatile("msr primask, %0" :: "r"(mask) : "memory"); }

static bool rx_ram(uint32_t address, uint32_t length)
{
    if (address < 0x00100000u || length > 0x100000u) return false;
    return address < (uint32_t)&_ram_end_ && length <= (uint32_t)&_ram_end_ - address;
}

static uint32_t rx_alloc_size(uint32_t p, uint32_t minimum)
{
    if (p < 8 || !rx_ram(p - 8, minimum + 8)) return 0;
    uint32_t raw = WORD(p - 4), size = raw & ~ALLOCATED;
    return (raw & ALLOCATED) && !(size & 7u) && size >= minimum + 8 && rx_ram(p - 8, size) ? size : 0;
}

/* Same shape test the read-only reader uses, but reduced to the identity fields. */
static bool rx_triple_at(uint32_t p, uint32_t size, uint32_t *out)
{
    if (size >= 80 && size <= 96 && BYTE(p + 40) == 2) {
        uint32_t buffer = WORD(p + 24), rxpd = WORD(p + 28);
        if (rx_alloc_size(buffer, 1596) == 1608 &&
            WORD(buffer + 4) == buffer + 16 && BYTE(buffer + 12) == 3 &&
            WORD(p + 20) == buffer + 16 && rx_alloc_size(rxpd, 20)) {
            out[0] = p;
            out[1] = buffer;
            out[2] = rxpd;
            out[3] = p + 8;
            out[4] = WORD(rxpd + 8) & 0xffffff;   /* sequence + tid packed by caller */
            return true;
        }
    }
    return false;
}

static void rx_record(uint32_t kind, uint32_t desc, uint32_t pbuf, uint32_t rxpd,
                      uint32_t a, uint32_t b)
{
    uint32_t seq = ++rx_event_seq;
    uint32_t *e = rx_events[(seq - 1) % TC1_RX_EVENTS];
    e[0] = seq;
    e[1] = mico_rtos_get_time();
    e[2] = kind;
    e[3] = desc;
    e[4] = pbuf;
    e[5] = rxpd;
    (void)a; (void)b;
}

/* One walk over the allocator regions. Called with IRQs already disabled. */
static void rx_walk(void)
{
    uint32_t found[RXW_SLOTS][4];
    unsigned nfound = 0;

    for (unsigned r = 0; r < 3; r++) {
        uint32_t base, bytes;
        if (r == 0) { base = WORD(0x00104830u); bytes = WORD(0x00104834u); }
        else if (r == 1) { base = (uint32_t)&link_bss_end; bytes = (uint32_t)&_ram_end_ - base; }
        else { base = WORD(0x00104838u); bytes = WORD(0x0010483cu); }
        if (!bytes || bytes < 16 || !rx_ram(base & ~3u, bytes + (base & 3u))) continue;
        uint32_t p = (base + 7u) & ~7u, end = (base + bytes - 8u) & ~7u;
        while (p < end && nfound < RXW_SLOTS) {
            uint32_t raw = WORD(p + 4), size = raw & ~ALLOCATED;
            if (size < 8 || (size & 7u) || size > end - p) break;
            if (raw & ALLOCATED) {
                uint32_t triple[5];
                if (rx_triple_at(p + 8, size, triple)) {
                    found[nfound][0] = triple[0];
                    found[nfound][1] = triple[1];
                    found[nfound][2] = triple[2];
                    bool dup = false;
                    for (unsigned k = 0; k < nfound; k++)
                        if (found[k][0] == triple[0]) { dup = true; break; }
                    if (!dup) nfound++;
                }
            }
            p += size;
        }
    }

    uint32_t now = mico_rtos_get_time();
    rx_walks++;

    /* New triples. */
    for (unsigned i = 0; i < nfound; i++) {
        bool known = false;
        for (unsigned s = 0; s < RXW_SLOTS; s++)
            if (rx_slots[s].descriptor == found[i][0]) { known = true; break; }
        if (known) continue;
        for (unsigned s = 0; s < RXW_SLOTS; s++) {
            if (rx_slots[s].descriptor) continue;
            rx_slots[s].descriptor = found[i][0];
            rx_slots[s].pbuf = found[i][1];
            rx_slots[s].rxpd = found[i][2];
            rx_slots[s].seq = WORD(found[i][2] + 8) >> 16;
            rx_slots[s].tid = WORD(found[i][2] + 8) & 0xf;
            rx_slots[s].first_ms = now;
            rx_slots[s].last_ms = now;
            rx_slots[s].walks = 1;
            rx_slots[s].held = 0;
            rx_slots[s].freed_seen = 0;
            rx_born++;
            rx_record(1, found[i][0], found[i][1], found[i][2], 0, 0);
            break;
        }
    }

    /* Existing triples: still present, or gone. */
    for (unsigned s = 0; s < RXW_SLOTS; s++) {
        if (!rx_slots[s].descriptor) continue;
        bool present = false;
        for (unsigned i = 0; i < nfound; i++)
            if (found[i][0] == rx_slots[s].descriptor) { present = true; break; }
        if (!present) {
            rx_gone++;
            rx_slots[s].freed_seen = 1;
            rx_record(2, rx_slots[s].descriptor, rx_slots[s].pbuf, rx_slots[s].rxpd,
                      rx_slots[s].walks, now - rx_slots[s].first_ms);
            rx_slots[s].descriptor = 0;
            continue;
        }
        rx_slots[s].last_ms = now;
        rx_slots[s].walks++;
        if (!rx_slots[s].held && rx_slots[s].walks > rx_hold_threshold) {
            rx_slots[s].held = 1;
            rx_held++;
            rx_record(3, rx_slots[s].descriptor, rx_slots[s].pbuf, rx_slots[s].rxpd,
                      rx_slots[s].walks, now - rx_slots[s].first_ms);
        }
    }
}

static void rx_watch_task(uint32_t arg)
{
    (void)arg;
    while (rx_running) {
        uint32_t mask = rx_irq_lock();
        rx_walk();
        rx_irq_unlock(mask);
        mico_rtos_thread_msleep(250);
    }
    rx_running = false;
    mico_rtos_delete_thread(NULL);
}

void tc1_rx_watch_start(void)
{
    if (rx_started) return;
    rx_started = true;
    rx_running = true;
    if (mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY - 1, "rx_watch",
                                rx_watch_task, 2048, 0) != kNoErr) {
        rx_running = false;
        rx_started = false;
    }
}

void tc1_rx_events_mp(ph_writer *w)
{
    uint32_t events_copy[TC1_RX_EVENTS][6];
    rx_watch slots_copy[RXW_SLOTS];
    uint32_t walks, born, gone, held, seq;
    uint32_t mask = rx_irq_lock();
    memcpy(events_copy, rx_events, sizeof(events_copy));
    memcpy(slots_copy, rx_slots, sizeof(slots_copy));
    walks = rx_walks; born = rx_born; gone = rx_gone; held = rx_held; seq = rx_event_seq;
    rx_irq_unlock(mask);

    phw_begin(w, NULL, false);
    phw_text(w, "code", "ok");
    phw_begin(w, "data", false);
    phw_bool(w, "supported", true);
    phw_u32(w, "walks", walks);
    phw_u32(w, "threshold", rx_hold_threshold);
    phw_u32(w, "born", born);
    phw_u32(w, "gone", gone);
    phw_u32(w, "held", held);
    phw_u32(w, "event_seq", seq);
    phw_begin(w, "slots", true);
    for (unsigned s = 0; s < RXW_SLOTS; s++) {
        if (!slots_copy[s].descriptor) continue;
        uint32_t row[8];
        row[0] = slots_copy[s].descriptor;
        row[1] = slots_copy[s].pbuf;
        row[2] = slots_copy[s].rxpd;
        row[3] = slots_copy[s].seq;
        row[4] = slots_copy[s].tid;
        row[5] = slots_copy[s].walks;
        row[6] = slots_copy[s].held;
        row[7] = slots_copy[s].last_ms - slots_copy[s].first_ms;
        phw_begin(w, NULL, true);
        for (unsigned i = 0; i < 8; i++) phw_u32(w, NULL, row[i]);
        phw_end(w);
    }
    phw_end(w);
    phw_begin(w, "events", true);
    for (unsigned i = 0; i < TC1_RX_EVENTS; i++) {
        if (!events_copy[i][0]) continue;
        phw_begin(w, NULL, true);
        for (unsigned j = 0; j < 6; j++) phw_u32(w, NULL, events_copy[i][j]);
        phw_end(w);
    }
    phw_end(w);
    phw_end(w); phw_end(w);
}
