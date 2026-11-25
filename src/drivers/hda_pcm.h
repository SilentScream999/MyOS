#ifndef WYZAR_HDA_PCM_H
#define WYZAR_HDA_PCM_H
// Included inside namespace hda, after the diagnostic transport/route helpers.
static uint64_t pcm_phys = 0;
static pid_t pcm_owner = 0;
static bool pcm_running = false, pcm_fault = false;
static HdaPcmRing pcm{};
static uint64_t pcm_progress_tick = 0;
// High-frequency controls must not use print(): it synchronously redraws and
// damages the Boot Log window. Keep these diagnostics in the memory boot log.
static void control_log_hex(const char* label,uint64_t value){
    char text[32];to_hex(value,text);
    klog_print(label);klog_putc('\n');klog_print(text);klog_putc('\n');
}

static bool firmware_identity() {
    return thinkpad_audio_identity(active.id,
        pci_cfg_read32(active.ecam, active.start, active.bus, active.dev, active.fn, 0x2c), codec_id);
}
static bool apply_volume() {
    AudioVolume desired;uint64_t revision;
    {TtyIrqGuard guard;desired=volume;revision=g_audio_mixer.revision;}
    if (!live_route.count) {g_audio_mixer.applied=revision;return true;}
    unsigned dac = live_route.nodes[live_route.count - 1], pin = live_route.nodes[0];
    const auto& widget = widgets[dac];
    if (widget.caps & 4) {
        unsigned amp = desired.amp(widget.output_amp);
        if (!set(dac, 0x300, 0xb000 | amp)) return false;
        uint32_t left, right;
        if (!verb(dac, 0xb00, 0xa000, left) || !verb(dac, 0xb00, 0x8000, right) ||
            (left & 255) != amp || (right & 255) != amp) return false;
        control_log_hex("Audio: DAC volume amp (verified)", amp);
    }
    unsigned enable = 0x40 | (hda_pin_kind(widgets[pin]) == 2 && (widgets[pin].pin_caps & 8) ? 0x80 : 0);
    if(!set(pin, 0x707, desired.muted || !desired.percent ? 0 : enable))return false;
    g_audio_mixer.applied=revision;return true;
}
static bool consume_volume_keys() {
    bool changed = false;
    {TtyIrqGuard guard;g_audio_hotkeys.tick(g_tick_count);}
    for (;;) {
        uint8_t key;
        { TtyIrqGuard guard; key = g_audio_hotkeys.pop(); }
        if (!key) break;
        control_log_hex("ThinkPad audio: deferred PS/2 multimedia make code", key);
        {TtyIrqGuard guard;g_audio_mixer.key(key);} changed = true;
    }
    if (g_audio_mixer.applied==g_audio_mixer.revision) return true;
    if(changed){control_log_hex("Audio: volume percent", volume.percent);control_log_hex("Audio: muted", volume.muted);}
    return apply_volume();
}
static void pcm_stop(bool fault = false) {
    if (!stop_stream()) { ready = false; bus_master(false); fault = true; }
    pcm_running = false; pcm_fault |= fault;
}
static void pcm_disconnect() {
    pcm_stop();
    if (live_route.count) {
        set(live_route.nodes[live_route.count - 1], 0x706, 0);
        set(live_route.nodes[0], 0x707, 0);
    }
    live_route = {}; pcm_owner = 0;
}
static bool pcm_service() {
    if (!pcm_running) return !pcm_fault;
    unsigned position = r32(stream + 4);
    unsigned status = r8(stream + 3);
    uint64_t previous = pcm.consumed;
    if (position > pcm.capacity || (status & 0x18) || !pcm.advance(position & ~3u, g_tick_count)) {
        print((char*)(status & 0x10 ? "Audio: PCM descriptor DMA error" :
            status & 8 ? "Audio: PCM hardware FIFO error" :
            position > pcm.capacity ? "Audio: PCM invalid DMA position" : "Audio: PCM service deadline missed"));
        log_hex("Audio: PCM error stream status", status);
        log_hex("Audio: PCM error link position", position);
        log_hex("Audio: PCM error service gap ms", g_tick_count - pcm.tick);
        log_hex("Audio: PCM error queued bytes", pcm.written > pcm.consumed ? pcm.written - pcm.consumed : 0);
        pcm_stop(true); return false;
    }
    if (pcm.consumed != previous) pcm_progress_tick = g_tick_count;
    if (g_tick_count - pcm_progress_tick > 500) {
        print((char*)"Audio: PCM DMA stalled"); pcm_stop(true); return false;
    }
    // Reclaim only complete consumed pages, before the producer may reuse them.
    while (pcm.cleared + pcm.block <= pcm.consumed) {
        uint64_t address = HHDM + pcm_phys + pcm.cleared % pcm.capacity;
        memset((void*)address, 0, pcm.block); dma_flush(address, pcm.block);
        pcm.cleared += pcm.block;
    }
    if (pcm.consumed >= pcm.written) {
        pcm_stop(); // End of queued samples; no old audio is replayed on wrap.
    }
    return !pcm_fault;
}
static bool pcm_start() {
    if (pcm_running || !pcm.written || (pcm.consumed && pcm.consumed >= pcm.written)) return !pcm_fault;
    pcm.position = 0; pcm.tick = pcm_progress_tick = g_tick_count;
    w8(stream + 3, 0x1c); fence(); w8(stream, 2);
    if (!wait([] { return r8(stream); }, 2, 2)) { pcm_fault = true; return false; }
    pcm_running = true; return true;
}
static bool pcm_buffer_valid(uint64_t address, uint64_t bytes) {
    if (!address || !bytes || bytes > 65536 || (bytes & 3) || address >= USERSPACE_END || bytes > USERSPACE_END - address) return false;
    for (uint64_t page = address & ~4095ULL; page <= ((address + bytes - 1) & ~4095ULL); page += 4096) {
        uint64_t table = read_cr3() & PTE_ADDR_MASK;
        for (int shift = 39; shift >= 12; shift -= 9) {
            uint64_t entry = ((uint64_t*)(HHDM + table))[(page >> shift) & 511];
            if ((entry & (PRESENT | USER)) != (PRESENT | USER)) return false;
            if (shift == 12 || ((shift == 30 || shift == 21) && (entry & HUGE))) break;
            table = entry & PTE_ADDR_MASK;
        }
    }
    return true;
}
static int64_t pcm_open(unsigned kind, unsigned requested_rate) {
    if (pcm_owner || !g_current_task) return -2;
    if (!kind) {
        // Prefer plugged headphones when pin presence detection is advertised.
        kind = available & 2 ? 1 : available & 4 ? 2 : 3;
        for (unsigned n = 1; n < 256; ++n) if (hda_pin_kind(widgets[n]) == 2 && (widgets[n].pin_caps & 4)) {
            uint32_t sense;
            if (verb(n, 0xf09, 0, sense) && (sense & 0x80000000u)) { kind = 2; break; }
        }
    }
    HdaRoute route{};
    if (kind > 3 || !(available & (1u << kind)) || !hda_select_output_route(widgets, kind, route)) return -3;
    unsigned rate = requested_rate == 44100 && (widgets[route.nodes[route.count - 1]].pcm_rates & (1u << 5)) ? 44100 : 48000;
    unsigned stream_format = rate == 44100 ? 0x4011 : format;
    if (firmware_identity() && thinkpad_audio::control(true, 1) < 0) {
        print((char*)"Audio: firmware unmute failed; inspect ThinkPad audio diagnostics"); return -1;
    }
    if (!stop_stream()) return -1;
    w8(stream, 1);
    bool self_clear = active.id == 0x26688086 && (codec_id >> 16) == 0x1af4 && !(r8(stream) & 1);
    if (!self_clear && !wait([] { return r8(stream); }, 1, 1)) return -1;
    w8(stream, 0);
    if (!wait([] { return r8(stream); }, 1, 0) || !configure_route(route, stream_format)) return -1;
    if (!pcm_phys) pcm_phys = alloc_phys_pages(pcm.capacity / pcm.block);
    if (!dma_address_ok(pcm_phys, pcm.capacity)) return -1;
    memset((void*)(HHDM + pcm_phys), 0, pcm.capacity); dma_flush(HHDM + pcm_phys, pcm.capacity);
    struct Descriptor { uint64_t address; uint32_t length, flags; };
    auto* bdl = (Descriptor*)(HHDM + dma_phys + 3072);
    constexpr unsigned descriptors = HdaPcmRing::capacity / HdaPcmRing::block;
    static_assert(3072 + descriptors * sizeof(Descriptor) <= 4096);
    for (unsigned i = 0; i < descriptors; ++i) bdl[i] = {pcm_phys + i * 4096, 4096, 0};
    dma_flush((uint64_t)bdl, descriptors * sizeof(Descriptor));
    w32(stream + 0x18, (uint32_t)(dma_phys + 3072)); w32(stream + 0x1c, (dma_phys + 3072) >> 32);
    w32(stream + 8, pcm.capacity); w16(stream + 0xc, descriptors - 1); w16(stream + 0x12, stream_format);
    w8(stream + 2, 0x10); w8(stream + 3, 0x1c);
    pcm = {}; pcm_fault = false; pcm_running = false;
    live_route = route; pcm_owner = g_current_task->pid;
    print((char*)"Audio: PCM stream opened");
    log_hex("Audio: PCM sample rate Hz", rate);
    log_hex("Audio: PCM DMA ring bytes", pcm.capacity);
    return requested_rate ? rate : 0;
}
static int64_t pcm_write(uint64_t address, unsigned bytes) {
    if (!pcm_buffer_valid(address, bytes)) return -4;
    unsigned done = 0;
    while (done < bytes) {
        if (g_current_task->interrupt_requested || !pcm_service() || !consume_volume_keys()) return -1;
        if (!pcm_running && pcm.consumed) {
            print((char*)"Audio: PCM underrun; playback stopped"); pcm_fault = true; return -1;
        }
        unsigned free = pcm.space();
        unsigned offset = pcm.written % pcm.capacity;
        unsigned count = bytes - done;
        if (count > free) count = free;
        if (count > pcm.capacity - offset) count = pcm.capacity - offset;
        if (!count) { sleep_ms(2); continue; }
        auto* destination = (int16_t*)(HHDM + pcm_phys + offset);
        const auto* source = (const int16_t*)(address + done);
        if (!(widgets[live_route.nodes[live_route.count - 1]].caps & 4)) {
            for (unsigned i = 0; i < count / 2; ++i) destination[i] = (int32_t)source[i] * volume.percent / 100;
        } else memcpy(destination, source, count);
        dma_flush(HHDM + pcm_phys + offset, count);
        pcm.written += count; done += count;
        if (pcm.written >= pcm.prefill && !pcm_start()) return -1;
    }
    return done;
}
// Shared controls: 6=status, 7=open(kind), 8=write(ptr,bytes), 9=drain,
// 10=close, 11=volume(0..100), 12=mute(0=off,1=on,2=toggle).
static int64_t control(uint64_t action, uint64_t arg, uint64_t bytes = 0) {
    if (action <= 5) {
        if (pcm_owner && (action == 1 || action == 2)) return -2;
        return control_legacy(action, arg);
    }
    if (action > 12 || (action == 11 && arg > 100) || (action == 12 && arg > 2)) return -4;
    if (action == 6) return volume.percent | ((uint64_t)volume.muted << 8);
    if (!attempted && control_legacy(0, 0) < 0) return -1;
    { TtyIrqGuard guard; if (busy) return -2; busy = true; }
    int64_t result = -1;
    if (action == 11 || action == 12) {
        {TtyIrqGuard guard;
        if (action == 11) g_audio_mixer.percent(arg);
        else g_audio_mixer.mute(arg);}
        result = apply_volume() ? 0 : -1;
    } else if (ready && action == 7) result = arg <= 3 && (!bytes || bytes == 44100 || bytes == 48000) ? pcm_open(arg, bytes) : -4;
    else if (pcm_owner && g_current_task && pcm_owner == g_current_task->pid) {
        if (action == 8) result = bytes <= 65536 ? pcm_write(arg, bytes) : -4;
        else if (action == 9) {
            result = pcm_start() ? 0 : -1;
            while (result == 0 && pcm_running) {
                if (g_current_task->interrupt_requested || !pcm_service() || !consume_volume_keys()) result = -1;
                if (pcm_running && result == 0) sleep_ms(2);
            }
            pcm_stop(result < 0); result = pcm_fault ? -1 : result;
            log_hex("Audio: PCM bytes submitted", pcm.written);
            print((char*)(result ? "Audio: PCM drain failed" : "Audio: PCM drain completed"));
        } else if (action == 10) { pcm_disconnect(); result = 0; }
    } else if (pcm_owner) result = -2;
    { TtyIrqGuard guard; busy = false; }
    return result;
}
static void poll_hotkeys() {
    { TtyIrqGuard guard; if (busy) return; busy = true; }
    if (pcm_owner) {
        bool alive = false;
        { TtyIrqGuard guard;
          Task* task = g_task_list;
          if (task) do { if (task->pid == pcm_owner && task->state != TaskState::DEAD && !task->interrupt_requested) alive = true; task = task->next; } while (task != g_task_list);
        }
        if (!alive) pcm_disconnect();
        else pcm_service();
    }
    consume_volume_keys();
    fn_probe::poll();
    { TtyIrqGuard guard; busy = false; }
}
static void service_task() {
    for (;;) {
        // Initialize discovered HDA controllers and the matching firmware
        // hotkey path without requiring playback or the capture command.
        // Explicit audio calls share the same busy/attempted guard. Failure
        // remains bounded and is not retried on every service tick.
        if(__atomic_load_n(&desktop_initialized,__ATOMIC_ACQUIRE) && device_count && !attempted)
            control_legacy(0,0);
        poll_hotkeys();sleep_ms(2);
    }
}
#endif
