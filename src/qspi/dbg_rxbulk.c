// バルクRX故障の切り分け: rxプログラム相当+cfg_rxで「確実に1」の入力を受信し、
//   (A) FIFOレベルポーリング+非ブロッキングget (DMA不使用)
//   (B) pio_sm_xfer_data (カーネルDMA)
// の2経路で読み比べる。期待値は全ワード0xFFFFFFFF。
//
// 入力源: side-setを全命令で1にしてCLK(GPIO12)を常時Highに保ち、
// in_pinsをCLK自身に向ける。パッドの駆動保持に依存しない
// (set_pinsによる静的駆動はsm_init後に保持されないことを確認済み)。
// CS(GPIO21)はcdevでHigh固定にしてチップを非選択にしておく。
//
//   Aが0x000000FFパターン → autopush 8bit挙動 = シフト設定の問題
//   Aが全FFでBが化ける   → カーネルDMA経路の問題 (dmesgのバースト長警告)
// ブロッキングget/putは使わない(kill時にRP1ファームウェアが詰まるため)。
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include "piolib.h"

#define CLK 12
#define CS  21

#define OFF_RX 4
#define OFF_RX_PARK 7

#define NBYTES 64   /* 16ワード */

static void hexdump(const char *label, const uint32_t *w, int nwords)
{
    printf("%s:", label);
    for (int i = 0; i < nwords; i++)
        printf(" %08X", w[i]);
    printf("\n");
}

static int check_all_ff(const char *label, const uint32_t *w, int nwords)
{
    int ok = 1;
    for (int i = 0; i < nwords; i++)
        if (w[i] != 0xFFFFFFFFu) ok = 0;
    printf("%s %s\n", label, ok ? "OK: 全ワード0xFFFFFFFF" : "NG");
    return ok;
}

// CS#をHigh固定で確保 (チップ非選択)
static int claim_cs_high(void)
{
    int chip_fd = open("/dev/gpiochip0", O_RDWR | O_CLOEXEC);
    if (chip_fd < 0)
        return -1;
    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof(req));
    req.offsets[0] = CS;
    req.num_lines = 1;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = 1;
    req.config.attrs[0].mask = 1;
    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        close(chip_fd);
        return -1;
    }
    close(chip_fd);
    return req.fd;
}

// rxプログラムをsm_initし、X=ビット数-1をロードして走らせる (rx_bulkと同じ手順)
static void start_rx(PIO pio, int sm, pio_sm_config *cfg, uint32_t nbits)
{
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_init(pio, sm, OFF_RX, cfg);
    pio_sm_put(pio, sm, nbits - 1);   /* TX FIFOは空なので非ブロッキングで安全 */
    pio_sm_exec(pio, sm, pio_encode_pull(false, true) | pio_encode_sideset(1, 1));
    pio_sm_exec(pio, sm, pio_encode_out(pio_x, 32) | pio_encode_sideset(1, 1));
    pio_sm_set_enabled(pio, sm, true);
}

int main(void)
{
    int cs_fd = claim_cs_high();
    if (cs_fd < 0) { printf("CS確保に失敗\n"); return 1; }

    PIO pio = pio_open(0);
    if (PIO_IS_ERR(pio)) { printf("pio_open err\n"); return 1; }
    int sm = pio_claim_unused_sm(pio, true);

    // rxプログラム (ドライバと同型だがside-set全1でCLK=High固定)
    static uint16_t rx[4];
    rx[0] = pio_encode_nop() | pio_encode_sideset(1, 1);
    rx[1] = pio_encode_in(pio_pins, 1) | pio_encode_sideset(1, 1);
    rx[2] = pio_encode_jmp_x_dec(OFF_RX) | pio_encode_sideset(1, 1);
    rx[3] = pio_encode_jmp(OFF_RX_PARK) | pio_encode_sideset(1, 1);
    pio_program_t p_rx = { .instructions = rx, .length = 4, .origin = OFF_RX };
    pio_add_program_at_offset(pio, &p_rx, OFF_RX);

    // cfg_rx相当 (w25n_pio.cのbuild_configsと同じシフト/ラップ設定)
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_sideset(&c, 1, false, false);
    sm_config_set_sideset_pins(&c, CLK);
    sm_config_set_in_pins(&c, CLK);   /* 常時HighのCLK自身を読む */
    sm_config_set_clkdiv(&c, 100.0f);
    sm_config_set_wrap(&c, OFF_RX, OFF_RX_PARK);
    sm_config_set_out_shift(&c, false, false, 32);
    sm_config_set_in_shift(&c, false, true, 32);

    pio_gpio_init(pio, CLK);
    pio_sm_init(pio, sm, OFF_RX, &c);
    pio_sm_set_consecutive_pindirs(pio, sm, CLK, 1, true);

    uint32_t words[NBYTES / 4];

    // (A) DMA不使用: FIFOポーリングで16ワード回収
    start_rx(pio, sm, &c, NBYTES * 8);
    memset(words, 0, sizeof(words));
    for (int i = 0; i < NBYTES / 4; i++) {
        int t;
        for (t = 0; t < 1000; t++) {
            if (pio_sm_get_rx_fifo_level(pio, sm) > 0)
                break;
            usleep(100);
        }
        if (t == 1000) { printf("(A) word%dでタイムアウト\n", i); break; }
        words[i] = pio_sm_get(pio, sm);
    }
    hexdump("(A) FIFOポーリング", words, NBYTES / 4);
    check_all_ff("(A)", words, NBYTES / 4);

    // (B) カーネルDMA: pio_sm_xfer_data
    if (pio_sm_config_xfer(pio, sm, PIO_DIR_FROM_SM, 4096, 4)) {
        printf("(B) config_xferに失敗\n");
        pio_close(pio);
        return 1;
    }
    start_rx(pio, sm, &c, NBYTES * 8);
    memset(words, 0, sizeof(words));
    if (pio_sm_xfer_data(pio, sm, PIO_DIR_FROM_SM, NBYTES, words)) {
        printf("(B) xfer_dataに失敗\n");
    } else {
        hexdump("(B) DMA", words, NBYTES / 4);
        check_all_ff("(B)", words, NBYTES / 4);
    }

    pio_close(pio);
    close(cs_fd);
    return 0;
}
