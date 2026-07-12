// Step 3 検証: 消去・書き込み・読み出しのラウンドトリップと、
// NANDセルの「二重書き込み=AND」特性(本プロジェクトの論理素子の核)を確認する。
// ブロック0(ページ0〜63)の内容は破壊される。
#include <stdio.h>
#include <string.h>
#include "w25n_pio.h"

#define PAGE_SIZE 2048

static int fail(const char *name, const char *what)
{
    fprintf(stderr, "%s: %s で失敗\n", name, what);
    return 1;
}

static int check_chip(const char *name, const w25n_pins_t *pins)
{
    w25n_dev_t *d = w25n_open(pins, 100.0f);  /* 1MHz */
    if (!d)
        return fail(name, "open");

    // 準備: 保護解除 + ECC無効(二重書き込みでECCバイトが矛盾するため)
    if (w25n_unprotect(d) < 0)
        return fail(name, "保護解除");
    if (w25n_set_ecc(d, 0) < 0)
        return fail(name, "ECC無効化");
    uint8_t sr1, sr2;
    w25n_read_status(d, 0xA0, &sr1);
    w25n_read_status(d, 0xB0, &sr2);
    printf("%s: SR-1=0x%02X SR-2=0x%02X (保護解除+ECCオフ)\n", name, sr1, sr2);

    // 1. ブロック0消去 → ページ0が全FFになること
    if (w25n_block_erase(d, 0) < 0)
        return fail(name, "block erase");
    static uint8_t rd[PAGE_SIZE];
    w25n_page_read(d, 0);
    w25n_read_buf(d, 0, rd, PAGE_SIZE);
    for (int i = 0; i < PAGE_SIZE; i++)
        if (rd[i] != 0xFF) {
            fprintf(stderr, "%s: 消去後 [%d]=0x%02X != FF\n", name, i, rd[i]);
            return 1;
        }
    printf("%s: erase OK (2048バイト全FF)\n", name);

    // 2. ページ0に2048バイトのパターンを書いて読み戻す
    static uint8_t wr[PAGE_SIZE];
    for (int i = 0; i < PAGE_SIZE; i++)
        wr[i] = (uint8_t)(i * 7 + 0x5A);
    if (w25n_program(d, 0, 0, wr, PAGE_SIZE) < 0)
        return fail(name, "program");
    w25n_page_read(d, 0);
    w25n_read_buf(d, 0, rd, PAGE_SIZE);
    if (memcmp(wr, rd, PAGE_SIZE)) {
        for (int i = 0; i < PAGE_SIZE; i++)
            if (wr[i] != rd[i]) {
                fprintf(stderr, "%s: 照合NG [%d] wr=%02X rd=%02X\n",
                        name, i, wr[i], rd[i]);
                break;
            }
        return 1;
    }
    printf("%s: program/read OK (2048バイト一致)\n", name);

    // 3. AND特性: ページ1にAを書き、消去せずBを重ね書き → A&B になること
    const uint8_t a[4] = { 0xF0, 0x3C, 0xAA, 0xFF };
    const uint8_t b[4] = { 0xCC, 0x0F, 0x55, 0x88 };
    if (w25n_program(d, 1, 0, a, sizeof(a)) < 0)
        return fail(name, "AND 1回目program");
    if (w25n_program(d, 1, 0, b, sizeof(b)) < 0)
        return fail(name, "AND 2回目program");
    w25n_page_read(d, 1);
    w25n_read_buf(d, 0, rd, sizeof(a));
    int ok = 1;
    for (unsigned i = 0; i < sizeof(a); i++) {
        uint8_t expect = a[i] & b[i];
        printf("%s: AND [%u] %02X & %02X = %02X (期待 %02X) %s\n",
               name, i, a[i], b[i], rd[i], expect, rd[i] == expect ? "OK" : "NG");
        if (rd[i] != expect)
            ok = 0;
    }
    if (!ok)
        return 1;

    // 後始末: テストデータを消しておく
    w25n_block_erase(d, 0);
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
