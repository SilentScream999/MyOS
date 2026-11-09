#ifndef WYZAR_USB_DEVICE_TREE_H
#define WYZAR_USB_DEVICE_TREE_H
// A root-port removal selects its full subtree; a hub-port removal selects only
// that hub's child branch. Siblings (including a mounted disk) remain untouched.
static bool usb_in_branch(USBDevice* dev, uint8_t root, USBDevice* hub, uint8_t port) {
    if (!hub) return dev->root_port_num == root;
    for (int depth = 0; dev && depth < 8; ++depth) {
        if (dev->parent_hub == hub && dev->port_num == port) return true;
        dev = dev->parent_hub;
    }
    return false;
}
#endif
