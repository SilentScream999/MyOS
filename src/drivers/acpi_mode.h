#ifndef WYZAR_ACPI_MODE_H
#define WYZAR_ACPI_MODE_H
#include <stdint.h>
// ACPI 6.5 4.7.3.1: firmware owns SCI_EN. Request ownership through the
// FADT's ACPI_ENABLE command, never write PM1_CNT to force the bit on.
// The caller must validate resources and keep an unhandled SCI masked.
struct AcpiModePorts {
    uint32_t smi;
    uint16_t pm1a,pm1b;
    uint8_t enable;
};
template<class Read,class Write,class Pause>
static int acpi_enter_mode(AcpiModePorts ports,Read read,Write write,Pause pause) {
    if(!ports.pm1a || ports.pm1a>65534 || ports.pm1b>65534) return -3;
    auto enabled=[&](){return (read(ports.pm1a)&1) && (!ports.pm1b || (read(ports.pm1b)&1));};
    if(enabled()) return 0;
    if(!ports.smi || ports.smi>65535 || !ports.enable) return -3;
    write((uint16_t)ports.smi,ports.enable);
    for(unsigned i=0;i<300;++i){
        if(enabled()) return 1;
        if(!pause()) break;
    }
    return enabled() ? 1 : -1;
}
#endif
