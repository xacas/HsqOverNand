// Step 2 検証: U2/U3のJEDEC IDとステータスレジスタをx1 SPIで読む
// 期待値: EF AA 22 (Winbond W25N02KV)
#include <stdio.h>
#include "w25n_pio.h"

static int check_chip(const char *name, const w25n_pins_t *pins)
{
    w25n_dev_t *d = w25n_open(pins, 100.0f);  /* 200MHz/100/2 = 1MHz */
    if (!d) {
        fprintf(stderr, "%s: openに失敗\n", name);
        return 1;
    }

    uint8_t id[3] = {0};
    w25n_jedec_id(d, id);
    int ok = (id[0] == 0xEF && id[1] == 0xAA && id[2] == 0x22);
    printf("%s: JEDEC ID = %02X %02X %02X  %s\n",
           name, id[0], id[1], id[2], ok ? "OK (W25N02KV)" : "NG!");

    uint8_t sr;
    static const struct { uint8_t addr; const char *label; } regs[] = {
        { 0xA0, "保護(SR-1)" }, { 0xB0, "設定(SR-2)" }, { 0xC0, "状態(SR-3)" },
    };
    for (unsigned i = 0; i < 3; i++) {
        w25n_read_status(d, regs[i].addr, &sr);
        printf("%s: %s = 0x%02X\n", name, regs[i].label, sr);
    }

    w25n_close(d);
    return ok ? 0 : 1;
}

int main(void)
{
    int rc = 0;
    rc |= check_chip("U2", &W25N_U2_PINS);
    rc |= check_chip("U3", &W25N_U3_PINS);
    return rc;
}
