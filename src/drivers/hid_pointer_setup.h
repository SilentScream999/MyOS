#ifndef WYZAR_HID_POINTER_SETUP_H
#define WYZAR_HID_POINTER_SETUP_H
#include "hid_mouse_report.h"
#include "hid_media_report.h"

// Returns true when the descriptor decides how to handle this interface.
// False allows the existing boot fallback for unavailable/unsupported layouts.
static bool usb_hid_pointer_prepare(USBDevice& dev, HIDInterface& interface) {
    interface.mouse_layout = nullptr;
    interface.media_layout = nullptr;
    interface.media_state = nullptr;
    interface.held_buttons = 0;
    uint16_t length = interface.report_descriptor_length;
    if (!length || length > 4096) return false;
    volatile uint8_t* descriptor = (volatile uint8_t*)alloc_table();
    for (uint16_t b = 0; b < length; ++b) descriptor[b] = 0;
    USBSetupPacket request = {};
    request.bmRequestType = 0x81; request.bRequest = 6;
    request.wValue = 0x2200; request.wIndex = interface.iface_num; request.wLength = length;
    if (!usb_control_ok(do_control_transfer(dev, &request, descriptor, length))) {
        print((char*)"[hid] Pointer report descriptor read failed; interface inactive.");
        interface.type = 3;
        return true;
    }
    HIDMouseLayout* layout = (HIDMouseLayout*)alloc_table();
    HIDMediaLayout* media = (HIDMediaLayout*)alloc_table();
    bool pointer_ready=hid_mouse_parse(descriptor,length,*layout);
    bool media_ready=hid_media_parse(descriptor,length,*media);
    if((pointer_ready || media_ready) && interface.boot_capable){
        request.bmRequestType=0x21;request.bRequest=0x0b;
        request.wValue=1;request.wLength=0;
        if(!usb_control_ok(do_control_transfer(dev,&request,nullptr,0))){
            interface.type=3;
            print((char*)"[hid] Report protocol selection failed; interface inactive.");
            return true;
        }
    }
    if(media_ready){
        // Consumer Control is report protocol, including Report IDs/bitmaps
        // and selector arrays. Do not force it into the mouse boot format.
        interface.media_layout=media;
        interface.media_state=(HIDMediaState*)alloc_table();
        *interface.media_state={};
    }
    if(!pointer_ready && media_ready){
        interface.type=4;
        print((char*)"[hid] Consumer media controls ready (descriptor-based reports).");
        return true;
    }
    if (!pointer_ready) {
        if (layout->parsed && !layout->pointer_collection) {
            interface.type = 3;
            print((char*)"[hid] Non-pointer report interface ignored.");
            return true;
        }
        print((char*)"[hid] Pointer descriptor unsupported; using existing fallback.");
        return false;
    }
    interface.mouse_layout = layout;
    interface.held_buttons = 0;
    interface.type = 2;
    print((char*)"[hid] Relative pointer ready (descriptor-based reports).");
    if(media_ready)print((char*)"[hid] Pointer interface also exposes media controls.");
    return true;
}
#endif
