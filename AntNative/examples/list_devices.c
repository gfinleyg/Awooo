/* Lists the attached ANT USB sticks through the C API.
 *
 * Build (VS 2022 Developer prompt, from AntNative):
 *   cl /nologo /W4 /Iinclude examples\list_devices.c build\Release\antnative.lib
 * and run it with build\Release\antnative.dll beside it or on PATH. */
#include <stdio.h>

#include "antnative.h"

int main(void) {
    ant_usb_device devices[8];
    int count = ant_list_devices(devices, 8);
    int i;

    if (count < 0) {
        fprintf(stderr, "ant_list_devices failed: %s\n", ant_last_error());
        return 1;
    }
    printf("AntNative API %d: %d stick(s)\n", ant_api_version(), count);
    for (i = 0; i < count && i < 8; ++i) {
        printf("  %u: %04X:%04X %s (serial %u)\n", devices[i].index, devices[i].vid, devices[i].pid,
               devices[i].has_product_description ? devices[i].product_description : "?",
               devices[i].serial_number);
    }
    return 0;
}
