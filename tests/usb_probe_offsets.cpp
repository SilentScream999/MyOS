#include <cstddef>
#include <cstdio>
#include "src/core/structures.h"
int main() {
    std::printf("%zu %zu %zu\n", offsetof(XHCIController,cr)+offsetof(Ring,phys),
                offsetof(USBDevice,ep0_ring), offsetof(Ring,phys));
}
