// Step 4 検証: DMAバルク転送の正しさと、クロックを上げたときの安定性・速度。
// ブロック1(ページ64〜127)を破壊する。
//
// 方針: 低速(1MHz)でページ64にパターンを書いておき、各clkdivで
//   1. JEDEC ID / 2. ページ64の読み戻し照合×3 / 3. 別ページへの書き込み+照合
// を行い、全部通った最速divでスループットを測る。
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "w25n_pio.h"

#define PAGE_SIZE 2048
#define BASE_PAGE 64   /* ブロック1 */

static double now_sec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void make_pattern(uint8_t *buf)
{
    for (int i = 0; i < PAGE_SIZE; i++)
        buf[i] = (uint8_t)(i * 13 + (i >> 8) * 101 + 0x3B);
}

static int check_chip(const char *name, const w25n_pins_t *pins)
{
    static uint8_t wr[PAGE_SIZE], rd[PAGE_SIZE];
    make_pattern(wr);

    w25n_dev_t *d = w25n_open(pins, 100.0f);  /* まず1MHz */
    if (!d) {
        fprintf(stderr, "%s: openに失敗\n", name);
        return 1;
    }
    w25n_unprotect(d);
    w25n_set_ecc(d, 0);

    // 基準データを低速で用意 (バルクTX/RXパス自体の検証も兼ねる)
    if (w25n_block_erase(d, BASE_PAGE) < 0)
        return 1;
    if (w25n_program(d, BASE_PAGE, 0, wr, PAGE_SIZE) < 0)
        return 1;
    w25n_page_read(d, BASE_PAGE);
    w25n_read_buf(d, 0, rd, PAGE_SIZE);
    if (memcmp(wr, rd, PAGE_SIZE)) {
        fprintf(stderr, "%s: 1MHzでのDMAラウンドトリップがNG\n", name);
        return 1;
    }
    printf("%s: DMAバルク転送OK (1MHz)\n", name);

    // クロックを上げながら検証。書き込みはdiv毎に別ページへ(NOP制限回避)
    static const float divs[] = { 50, 20, 10, 8, 6, 4, 3, 2 };
    float best = 100;
    for (unsigned k = 0; k < sizeof(divs) / sizeof(divs[0]); k++) {
        float div = divs[k];
        w25n_set_clkdiv(d, div);
        const char *fail = NULL;

        uint8_t id[3];
        w25n_jedec_id(d, id);
        if (id[0] != 0xEF || id[1] != 0xAA || id[2] != 0x22)
            fail = "JEDEC ID";

        for (int r = 0; !fail && r < 3; r++) {
            w25n_page_read(d, BASE_PAGE);
            memset(rd, 0, PAGE_SIZE);
            w25n_read_buf(d, 0, rd, PAGE_SIZE);
            if (memcmp(wr, rd, PAGE_SIZE))
                fail = "読み戻し照合";
        }

        if (!fail) {
            uint32_t page = BASE_PAGE + 1 + k;
            if (w25n_program(d, page, 0, wr, PAGE_SIZE) < 0) {
                fail = "program";
            } else {
                w25n_page_read(d, page);
                w25n_read_buf(d, 0, rd, PAGE_SIZE);
                if (memcmp(wr, rd, PAGE_SIZE))
                    fail = "書き込み照合";
            }
        }

        printf("%s: div=%-3g  %s%s\n",
               name, div,
               fail ? "NG: " : "OK", fail ? fail : "");
        if (!fail && div < best)
            best = div;
    }

    // 安全マージンとして安定最速の1段遅いdivを推奨とし、スループット測定
    float run = 100;
    for (unsigned k = 0; k < sizeof(divs) / sizeof(divs[0]); k++)
        if (divs[k] > best)
            run = divs[k];  /* 降順配列なので最後に残るのが直前段 */
    w25n_set_clkdiv(d, run);

    const int N = 200;
    double t0 = now_sec();
    for (int i = 0; i < N; i++) {
        w25n_page_read(d, BASE_PAGE);
        w25n_read_buf(d, 0, rd, PAGE_SIZE);
    }
    double dt = now_sec() - t0;
    printf("%s: 推奨div=%g  ページ読み出し %.0f回/秒 (%.1f KB/s, 13h+03h 2048B)\n",
           name, run, N / dt, N * PAGE_SIZE / dt / 1024);

    // 参考: バイト毎ioctl転送(2043B=4の倍数でない長さで非バルクパスに落とす)
    t0 = now_sec();
    for (int i = 0; i < 3; i++) {
        w25n_page_read(d, BASE_PAGE);
        w25n_read_buf(d, 0, rd, 2043);
    }
    dt = now_sec() - t0;
    printf("%s: (参考)バイト毎転送だと %.1f回/秒\n", name, 3 / dt);

    w25n_block_erase(d, BASE_PAGE);
    w25n_close(d);
    return 0;
}

int main(void)
{
    int rc = 0;
    rc |= check_chip("U2", &W25N_U2_PINS);
    rc |= check_chip("U3", &W25N_U3_PINS);
    printf(rc ? "FAILED\n" : "ALL OK\n");
    return rc;
}
