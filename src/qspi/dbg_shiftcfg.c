// 実ピン(U2)でシフト閾値の切り替えが効くかを検証。
// INピンをOUTピン(IO0)に向けたループバックで既知パターンを読み戻す:
//   8bit閾値:  put 0xDEADBEEF → OSRは8bitで空扱い → got=0x000000DE
//   32bit閾値: put 0xDEADBEEF → 32bit全部シフト   → got=0xDEADBEEF
// ブロッキングGETは使わない(FIFOレベルポーリング+タイムアウト)。
// ブロッキングGET中のkillはRP1ファームウェアを恒久的に詰まらせるため。
#include <stdio.h>
#include <unistd.h>
#include "piolib.h"

#define CLK 12
#define IO0 19

// RXにワードが来るまでポーリングし、非ブロッキングgetで返す
static int get_polled(PIO pio, int sm, uint32_t *out)
{
    for (int i = 0; i < 1000; i++) {          /* 最大 ~100ms */
        if (pio_sm_get_rx_fifo_level(pio, sm) > 0) {
            *out = pio_sm_get(pio, sm);
            return 0;
        }
        usleep(100);
    }
    return -1;
}

static void run_case(PIO pio, int sm, pio_sm_config *c, const char *label,
                     uint32_t put_val, uint32_t expect)
{
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_init(pio, sm, 0, c);
    pio_sm_set_enabled(pio, sm, true);
    if (pio_sm_is_tx_fifo_full(pio, sm)) {
        printf("%s: TX FIFOが満杯 (SM停止?)\n", label);
        return;
    }
    pio_sm_put(pio, sm, put_val);
    uint32_t got;
    if (get_polled(pio, sm, &got) < 0) {
        printf("%s: put=0x%08X → RXタイムアウト\n", label, put_val);
        return;
    }
    printf("%s: put=0x%08X got=0x%08X expect=0x%08X %s\n",
           label, put_val, got, expect, got == expect ? "OK" : "NG");
}

int main(void)
{
    PIO pio = pio_open(0);
    if (PIO_IS_ERR(pio)) { printf("pio_open err\n"); return 1; }
    int sm = pio_claim_unused_sm(pio, true);

    static uint16_t fdx[2];
    fdx[0] = pio_encode_out(pio_pins, 1) | pio_encode_sideset(1, 0);
    fdx[1] = pio_encode_in(pio_pins, 1) | pio_encode_sideset(1, 1);
    pio_program_t p = { .instructions = fdx, .length = 2, .origin = 0 };
    pio_add_program_at_offset(pio, &p, 0);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, 0, 1);
    sm_config_set_sideset(&c, 1, false, false);
    sm_config_set_sideset_pins(&c, CLK);
    sm_config_set_out_pins(&c, IO0, 1);
    sm_config_set_in_pins(&c, IO0);   /* ループバック: 自分の駆動値を読む */
    sm_config_set_out_shift(&c, false, true, 8);
    sm_config_set_in_shift(&c, false, true, 8);
    sm_config_set_clkdiv(&c, 100.0f);

    pio_gpio_init(pio, CLK);
    pio_gpio_init(pio, IO0);
    pio_sm_init(pio, sm, 0, &c);
    pio_sm_set_consecutive_pindirs(pio, sm, CLK, 1, true);
    pio_sm_set_consecutive_pindirs(pio, sm, IO0, 1, true);

    run_case(pio, sm, &c, "8bit閾値 (基準)        ", 0xA5u << 24, 0x000000A5);

    pio_sm_config c32 = c;
    sm_config_set_out_shift(&c32, false, true, 32);
    sm_config_set_in_shift(&c32, false, true, 32);
    run_case(pio, sm, &c32, "32bit閾値 (sm_init)    ", 0xDEADBEEFu, 0xDEADBEEF);

    run_case(pio, sm, &c, "8bit閾値に戻す (sm_init)", 0x5Cu << 24, 0x0000005C);

    // sm_initを使わずSET_CONFIGだけで切り替わるかも確認
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_set_config(pio, sm, &c32);
    pio_sm_set_enabled(pio, sm, true);
    if (!pio_sm_is_tx_fifo_full(pio, sm)) {
        pio_sm_put(pio, sm, 0xCAFEBABEu);
        uint32_t got;
        if (get_polled(pio, sm, &got) == 0)
            printf("32bit閾値 (set_config)  : put=0xCAFEBABE got=0x%08X expect=0xCAFEBABE %s\n",
                   got, got == 0xCAFEBABEu ? "OK" : "NG");
        else
            printf("32bit閾値 (set_config)  : RXタイムアウト\n");
    }

    pio_close(pio);
    return 0;
}
