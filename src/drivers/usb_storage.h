#ifndef WYZAR_USB_STORAGE_H
#define WYZAR_USB_STORAGE_H
// Included by usbhelpers.h after ring/control helpers. USB-IF BOT 1.0:
// https://www.usb.org/sites/default/files/usbmassbulk_10.pdf
// One LUN, transparent SCSI, READ/WRITE(10), 512-byte sectors; no UAS.
struct usb_storage {
    USBDevice* dev;
    Ring in_ring, out_ring;
    uint8_t in_dci, out_dci;
    uint8_t* dma; // one physically contiguous, page-aligned bounce buffer
    uint32_t tag;
    block_device disk;
};
static usb_storage usb_disks[8] = {};
static int usb_disk_count = 0;

static bool usb_storage_wait(USBDevice* dev, uint32_t type, uint64_t pointer, uint8_t dci, uint32_t bytes) {
    uint64_t end = _usb_rdtsc() + _usb_tsc_hz() * 2;
    while (_usb_rdtsc() < end) {
        USB_Response r = get_usb_response_raw(dev->hc, 1);
        if (!r.gotresponse) continue;
        if (r.type == type && r.event->parameter == pointer &&
            ((r.ctrl >> 24) & 255) == dev->slot_id &&
            (type != 32 || ((r.ctrl >> 16) & 31) == dci)) {
            return (r.event->status >> 24) == 1 &&
                   (type != 32 || (r.event->status & 0xffffff) == 0);
        }
        if (!usb_defer(dev->hc, r)) return false;
    }
    (void)bytes;
    return false;
}
static bool usb_storage_bulk(usb_storage* s, bool in, uint32_t bytes) {
    Ring* ring = in ? &s->in_ring : &s->out_ring;
    uint8_t dci = in ? s->in_dci : s->out_dci;
    uint64_t pointer = ring->phys + ring->enq * 16;
    ring_push_cmd(ring, (1u << 10) | (1u << 5), (uint64_t)s->dma - HHDM, bytes);
    __asm__ volatile("sfence" ::: "memory");
    s->dev->hc->doorbell32[s->dev->slot_id] = dci;
    return usb_storage_wait(s->dev, 32, pointer, dci, bytes);
}
static uint32_t usb_scsi_be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void usb_scsi_put32(uint8_t* p, uint32_t n) {
    p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n;
}
// Returns 0 success, 1 command failure (REQUEST SENSE is allowed), -1 transport
// failure. A failed transport is offlined rather than reusing live DMA/rings.
static int usb_storage_command(usb_storage* s, const uint8_t* cdb, uint8_t cdb_len, bool in, void* data, uint32_t bytes) {
    if (!s->disk.online || s->dev->hc->needsResetting || bytes > 512) return -1;
    uint32_t tag = ++s->tag;
    for (int i = 0; i < 31; ++i) s->dma[i] = 0;
    s->dma[0] = 'U'; s->dma[1] = 'S'; s->dma[2] = 'B'; s->dma[3] = 'C';
    for (int i = 0; i < 4; ++i) { s->dma[4 + i] = tag >> (8 * i); s->dma[8 + i] = bytes >> (8 * i); }
    s->dma[12] = in ? 0x80 : 0; s->dma[14] = cdb_len;
    for (int i = 0; i < cdb_len; ++i) s->dma[15 + i] = cdb[i];
    bool ok = usb_storage_bulk(s, false, 31);
    if (ok && bytes) {
        if (!in) memcpy(s->dma, data, bytes);
        ok = usb_storage_bulk(s, in, bytes);
        if (ok && in) memcpy(data, s->dma, bytes);
    }
    if (ok) ok = usb_storage_bulk(s, true, 13);
    if (ok) {
        ok = s->dma[0] == 'U' && s->dma[1] == 'S' && s->dma[2] == 'B' && s->dma[3] == 'S';
        for (int i = 0; i < 4; ++i) if (s->dma[4 + i] != (uint8_t)(tag >> (8 * i))) ok = false;
        uint32_t residue = (uint32_t)s->dma[8] | (uint32_t)s->dma[9] << 8 | (uint32_t)s->dma[10] << 16 | (uint32_t)s->dma[11] << 24;
        if (ok && residue <= bytes && s->dma[12] == 1) return 1;
        ok = ok && residue == 0 && s->dma[12] == 0;
    }
    if (!ok) {
        s->disk.online = false;
        print((char*)"[storage] USB transport failed; disk offline until reboot.");
        return -1;
    }
    return 0;
}
static bool usb_disk_read(block_device* d, uint64_t lba, void* data) {
    usb_storage* s = (usb_storage*)d->context;
    uint8_t cdb[10] = {0x28}; usb_scsi_put32(cdb + 2, (uint32_t)lba); cdb[8] = 1;
    return usb_storage_command(s, cdb, 10, true, data, 512) == 0;
}
static bool usb_disk_write(block_device* d, uint64_t lba, const void* data) {
    usb_storage* s = (usb_storage*)d->context;
    uint8_t cdb[10] = {0x2a}; usb_scsi_put32(cdb + 2, (uint32_t)lba); cdb[8] = 1;
    return usb_storage_command(s, cdb, 10, false, (void*)data, 512) == 0;
}
static bool usb_disk_flush(block_device* d) {
    uint8_t cdb[10] = {0x35};
    return usb_storage_command((usb_storage*)d->context, cdb, 10, false, nullptr, 0) == 0;
}
static bool usb_storage_is_device(USBDevice* dev) {
    for (int i = 0; i < usb_disk_count; ++i) if (usb_disks[i].dev == dev) return true;
    return false;
}
static void usb_storage_reset_controller(XHCIController* hc) {
    // Keep old block_device objects stable: mounted vnodes must never target a
    // different device after reconnect or slot reuse.
    for (int i = 0; i < usb_disk_count; ++i)
        if (usb_disks[i].dev->hc == hc) usb_disks[i].disk.online = false;
    usb_deferred_events* q = usb_pending_for(hc); q->head = q->count = 0;
}
static bool usb_storage_configure(USBDevice* dev, uint8_t in_ep, uint16_t in_mps, uint8_t out_ep, uint16_t out_mps) {
    if (usb_disk_count == 8 || !in_ep || !out_ep || !in_mps || !out_mps) return false;
    usb_storage* s = &usb_disks[usb_disk_count++];
    s->dev = dev; s->in_dci = in_ep * 2 + 1; s->out_dci = out_ep * 2; s->tag = 0;
    s->dma = (uint8_t*)alloc_table();
    ring_init(&s->in_ring); ring_init(&s->out_ring);
    size_t stride = (dev->hc->hcc1 & 4) ? 64 : 32;
    volatile uint32_t* ctx = (volatile uint32_t*)alloc_table();
    for (int j = 0; j < 1024; ++j) ctx[j] = 0;
    ctx[1] = 1u | (1u << s->in_dci) | (1u << s->out_dci);
    volatile uint32_t* slot = (volatile uint32_t*)((uint64_t)ctx + stride);
    volatile uint32_t* old = (volatile uint32_t*)dev->device_ctx;
    for (size_t j = 0; j < stride / 4; ++j) slot[j] = old[j];
    uint8_t max_dci = s->in_dci > s->out_dci ? s->in_dci : s->out_dci;
    slot[0] = (slot[0] & ~(31u << 27)) | (uint32_t)max_dci << 27;
    for (int direction = 0; direction < 2; ++direction) {
        uint8_t dci = direction ? s->in_dci : s->out_dci;
        uint16_t mps = direction ? in_mps : out_mps;
        Ring* r = direction ? &s->in_ring : &s->out_ring;
        volatile uint32_t* ep = (volatile uint32_t*)((uint64_t)ctx + stride * (dci + 1));
        ep[1] = (3u << 1) | ((direction ? 6u : 2u) << 3) | (uint32_t)mps << 16;
        ep[2] = (uint32_t)r->phys | 1; ep[3] = r->phys >> 32; ep[4] = 512;
    }
    uint64_t command = dev->hc->cr.phys + dev->hc->cr.enq * 16;
    ring_push_cmd(&dev->hc->cr, (12u << 10) | (dev->slot_id << 24), (uint64_t)ctx - HHDM, 0);
    dev->hc->doorbell32[0] = 0;
    if (!usb_storage_wait(dev, 33, command, 0, 0)) return false;
    s->disk.context = s; s->disk.online = true; s->disk.sectors = 0;
    s->disk.read = usb_disk_read; s->disk.write = usb_disk_write; s->disk.flush = usb_disk_flush;
    // Unit attention after USB reset is expected. Consume sense and retry TUR.
    bool ready = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        uint8_t tur[6] = {}, sense[6] = {3, 0, 0, 0, 18, 0}, data[18];
        int status = usb_storage_command(s, tur, 6, false, nullptr, 0);
        if (status == 0) { ready = true; break; }
        if (status < 0 || usb_storage_command(s, sense, 6, true, data, 18) != 0) break;
        // Busy wait here: enumeration may be under storage_guard.
        uint64_t end = _usb_rdtsc() + _usb_tsc_hz() / 10;
        while (_usb_rdtsc() < end) __asm__ volatile("pause");
    }
    uint8_t cdb[10] = {0x25}, capacity[8];
    if (!ready || usb_storage_command(s, cdb, 10, true, capacity, 8) != 0 ||
        usb_scsi_be32(capacity + 4) != 512 || usb_scsi_be32(capacity) == 0xffffffff) {
        s->disk.online = false; return false;
    }
    s->disk.sectors = (uint64_t)usb_scsi_be32(capacity) + 1;
    print((char*)"[storage] USB SCSI disk ready (512-byte sectors).");
    return true;
}
#endif
