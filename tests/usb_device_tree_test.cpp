#include <cassert>
#include <cstdio>
#include "src/core/structures.h"
#include "src/drivers/usb_device_tree.h"
int main() {
    USBDevice hub={},disk={},mouse={},nested={},keyboard={},other_root={};
    hub.root_port_num=4; hub.port_num=4;
    disk.root_port_num=mouse.root_port_num=nested.root_port_num=keyboard.root_port_num=4;
    disk.parent_hub=mouse.parent_hub=nested.parent_hub=&hub;
    disk.port_num=1; mouse.port_num=2; nested.port_num=3;
    keyboard.parent_hub=&nested; keyboard.port_num=1;
    other_root.root_port_num=5;
    assert(usb_in_branch(&mouse,0,&hub,2));
    assert(!usb_in_branch(&disk,0,&hub,2));
    assert(!usb_in_branch(&keyboard,0,&hub,2));
    assert(usb_in_branch(&nested,0,&hub,3));
    assert(usb_in_branch(&keyboard,0,&hub,3));
    assert(!usb_in_branch(&hub,0,&hub,3));
    assert(usb_in_branch(&keyboard,0,&nested,1));
    assert(!usb_in_branch(&disk,0,&nested,1));
    assert(usb_in_branch(&disk,4,nullptr,0));
    assert(usb_in_branch(&keyboard,4,nullptr,0));
    assert(!usb_in_branch(&other_root,4,nullptr,0));
    keyboard.parent_hub=&keyboard;
    assert(!usb_in_branch(&keyboard,0,&hub,3));
    puts("USB tree: selective child removal, nested branches and sibling disk isolation passed");
}
