#include <cassert>
#include <initializer_list>
#include "../src/drivers/hda_topology.h"

static HdaWidget widgets[256];
int main() {
    HdaConnections list{};
    assert(list.append(2, false));
    assert(list.append(0x85, false));
    assert(list.count == 4 && list.nodes[0] == 2 && list.nodes[3] == 5);
    assert(!list.append(0x87, false)); // consecutive ranges are malformed
    assert(list.append(9, false));
    assert(list.nodes[4] == 9); // selector index is expanded, not packed index
    HdaConnections bad{};
    assert(!bad.append(0x82, false));
    assert(!bad.append(0, false));
    assert(!bad.append(256, true));
    assert(bad.append(1, true));
    assert(bad.append(0x8004, true));
    assert(bad.count == 4 && bad.nodes[3] == 4);
    assert(!bad.append(0x8003, true));
    HdaConnections full{};
    assert(full.append(1, false));
    assert(!full.append(0xff, false)); // bounded output, no overflow
    assert(full.count == 1);

    // Pin -> selector -> mixer -> DAC, including a cyclic dead-end input.
    for (unsigned nid : {2u, 3u, 4u, 5u, 6u}) widgets[nid].present = true;
    widgets[2].caps = 1; widgets[2].pcm = true;
    widgets[3].caps = 2u << 20;
    widgets[4].caps = 3u << 20;
    widgets[5].caps = 4u << 20;
    widgets[6].caps = 3u << 20;
    assert(widgets[5].inputs.append(4, false));
    assert(widgets[4].inputs.append(6, false));
    assert(widgets[4].inputs.append(3, false));
    assert(widgets[6].inputs.append(4, false));
    assert(widgets[3].inputs.append(2, false));
    HdaRoute route{};
    assert(hda_find_route(widgets, 5, route));
    assert(route.count == 4 && route.nodes[3] == 2 && route.indices[1] == 1);
    widgets[2].pcm = false;
    route = {}; assert(!hda_find_route(widgets, 5, route) && !route.count);
    widgets[2].pcm = true; widgets[2].caps |= 1u << 9;
    route = {}; assert(!hda_find_route(widgets, 5, route)); // digital converter
    widgets[5].pin_caps = 1u << 4;
    widgets[5].config = 1u << 20;
    assert(hda_pin_kind(widgets[5]) == 1);
    widgets[5].config = 2u << 20;
    assert(hda_pin_kind(widgets[5]) == 2);
    widgets[5].config = 0;
    assert(hda_pin_kind(widgets[5]) == 3);
    widgets[5].config = 1u << 30;
    assert(!hda_pin_kind(widgets[5])); // no physical connection
    widgets[5].config = 0; widgets[5].caps |= 1u << 9;
    assert(!hda_pin_kind(widgets[5]));
    assert(hda_amp_gain(0x80001f08) == 8);
    assert(hda_amp_gain(0x8000081f) == 8);
    assert(hda_amp_gain(0) == 0);

    // Explicit alternate-DAC tests retain the physical speaker pin and choose
    // only its advertised connections, as on the Conexant two-DAC topology.
    for (auto& w : widgets) w = {};
    for (unsigned nid : {0x10u, 0x11u, 0x17u}) widgets[nid].present = true;
    widgets[0x10].caps = widgets[0x11].caps = 1;
    widgets[0x10].pcm = widgets[0x11].pcm = true;
    auto& speaker = widgets[0x17];
    speaker.caps = 4u << 20; speaker.pin_caps = 1u << 4; speaker.config = 1u << 20;
    assert(speaker.inputs.append(0x10, false) && speaker.inputs.append(0x11, false));
    route = {}; assert(hda_select_output_route(widgets, 1, route));
    assert(route.nodes[0] == 0x17 && route.nodes[1] == 0x10 && route.indices[0] == 0);
    assert(hda_select_output_route(widgets, 1, route, true));
    assert(route.nodes[0] == 0x17 && route.nodes[1] == 0x11 && route.indices[0] == 1);
    widgets[0x11].caps |= 1u << 9;
    assert(!hda_select_output_route(widgets, 1, route, true) && !route.count);
    widgets[0x11].caps = 1; widgets[0x11].pcm = false;
    assert(!hda_select_output_route(widgets, 1, route, true));
    assert(!hda_select_output_route(widgets, 2, route));
}
