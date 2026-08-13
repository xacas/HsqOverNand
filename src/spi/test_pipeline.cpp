// HW SPI(spidev)経由でのW25N02KV疎通確認と、複数チップへの非同期Program
// (issue/wait分割)によるパイプライン効果を測定するベンチマーク。
//
// 実チップのブロック1個分(グループ = 128KB×チップ数)を破壊的に書き換える。
// 使い方: ./spiPipelineTest /dev/spidev0.0 [/dev/spidev1.0 ...]
//   1チップ指定時は疎通確認とAND特性検証のみ(パイプライン比較はスキップ)
//   2チップ以上でsync(1チップ順次)とpipelined(全チップ非同期発行)を比較する
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "W25NSpiFlashDevice.hpp"
#include "w25n_spidev.h"

#define PAGE_SIZE W25N_SPI_PAGE_SIZE

static double now_sec()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void make_pattern(uint8_t* buf, unsigned seed)
{
    for (int i = 0; i < (int)PAGE_SIZE; i++)
        buf[i] = (uint8_t)(i * 13 + (i >> 8) * 101 + seed);
}

// startGroup以降で、全チップの物理ブロックが良品であるグループ番号を探す
// (グループ番号=物理ブロック番号。W25NSpiFlashDevice.hppのコメント参照)
static int find_good_group(W25NSpiFlashDevice& dev, uint32_t startGroup)
{
    for (uint32_t g = startGroup; g < W25N_SPI_NUM_BLOCKS; g++)
        if (dev.isGroupGood(g))
            return (int)g;
    return -1;
}

// 指定チップ1つの中で、startBlock以降にrunLen個連続する良品ブロックの
// 先頭ブロック番号を探す(単一チップベンチマークの書き込み範囲確保用)
static int find_good_run(W25NSpiFlashDevice& dev, size_t chipIdx, uint32_t runLen, uint32_t startBlock)
{
    for (uint32_t b = startBlock; b + runLen <= W25N_SPI_NUM_BLOCKS; b++) {
        bool ok = true;
        for (uint32_t k = 0; k < runLen && ok; k++)
            if (dev.isBadBlock(chipIdx, b + k))
                ok = false;
        if (ok)
            return (int)b;
    }
    return -1;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "使い方: %s /dev/spidevB.C [/dev/spidevB2.C2 ...]\n", argv[0]);
        return 1;
    }
    uint32_t hz = 20000000;
    if (const char* e = getenv("W25N_SPI_HZ")) hz = (uint32_t)strtoul(e, nullptr, 10);

    std::vector<std::string> paths;
    for (int i = 1; i < argc; i++) paths.push_back(argv[i]);

    printf("=== JEDEC ID疎通確認 (%.1fMHz) ===\n", hz / 1e6);
    for (auto& p : paths) {
        if (!W25NSpiFlashDevice::probe(p, hz)) {
            fprintf(stderr, "%s: JEDEC IDが取得できない(配線/デバイスパスを確認)\n", p.c_str());
            return 1;
        }
        printf("%s: OK (W25N02KV)\n", p.c_str());
    }

    W25NSpiFlashDevice dev;
    if (!dev.open(paths, hz)) {
        fprintf(stderr, "open失敗\n");
        return 1;
    }
    size_t n = dev.chipCount();
    printf("チップ数=%zu, 合計容量=%.0fMB\n", n, dev.size() / 1e6);

    // 1グループ(128KB×チップ数)ぶん使う。グループ境界を跨いだ複数ページ
    // 書き込みを試すため、テストページ数は1グループ分(64ページ×チップ数)。
    // グループ番号=全チップ共通の物理ブロック番号なので、1チップでも
    // 不良ブロックマークがあるグループは使わずスキャンして避ける
    // (グループ0は念のためスキップして探索を始める)
    const uint32_t groupPages = 64 * (uint32_t)n;
    printf("\n=== 不良ブロックスキャン ===\n");
    int group = find_good_group(dev, 1);
    if (group < 0) {
        fprintf(stderr, "全チップで良品なグループが見つからない\n");
        return 1;
    }
    printf("使用グループ=%d (全%zuチップで不良ブロックマークなしを確認)\n", group, n);
    const uint32_t basePage = (uint32_t)group * groupPages;
    const uint32_t baseOff = basePage * PAGE_SIZE;
    const uint32_t groupBytes = groupPages * PAGE_SIZE;

    printf("\n=== 正しさ確認 (ページ%u〜%u, %u ページ) ===\n", basePage, basePage + groupPages - 1, groupPages);
    if (!dev.erase(baseOff, groupBytes)) {
        fprintf(stderr, "erase失敗\n");
        return 1;
    }
    std::vector<uint8_t> wr(groupBytes), rd(groupBytes);
    for (uint32_t p = 0; p < groupPages; p++)
        make_pattern(&wr[p * PAGE_SIZE], p);
    if (!dev.write(baseOff, wr.data(), groupBytes)) {
        fprintf(stderr, "write失敗\n");
        return 1;
    }
    if (!dev.read(baseOff, rd.data(), groupBytes)) {
        fprintf(stderr, "read失敗\n");
        return 1;
    }
    if (memcmp(wr.data(), rd.data(), groupBytes) != 0) {
        fprintf(stderr, "ラウンドトリップ不一致\n");
        return 1;
    }
    printf("ラウンドトリップOK\n");

    // AND特性: 同一ページへ2回書き込むと結果はビット単位ANDになる
    // (セル=書込値のANDというNAND-SUBLEQ計算機の前提そのもの)
    {
        std::vector<uint8_t> a(PAGE_SIZE), b(PAGE_SIZE), got(PAGE_SIZE), expect(PAGE_SIZE);
        make_pattern(a.data(), 1);
        make_pattern(b.data(), 99);
        if (!dev.erase(baseOff, groupBytes)) { fprintf(stderr, "erase失敗\n"); return 1; }
        if (!dev.write(baseOff, a.data(), PAGE_SIZE)) { fprintf(stderr, "write失敗\n"); return 1; }
        if (!dev.write(baseOff, b.data(), PAGE_SIZE)) { fprintf(stderr, "write(2回目)失敗\n"); return 1; }
        if (!dev.read(baseOff, got.data(), PAGE_SIZE)) { fprintf(stderr, "read失敗\n"); return 1; }
        for (size_t i = 0; i < PAGE_SIZE; i++) expect[i] = a[i] & b[i];
        if (memcmp(got.data(), expect.data(), PAGE_SIZE) != 0) {
            fprintf(stderr, "AND特性が一致しない\n");
            return 1;
        }
        printf("AND特性OK (二重書き込み=ビットAND)\n");
    }

    if (n < 2) {
        printf("\nチップ数が1のためパイプライン比較はスキップします\n");
        return 0;
    }

    // === ベンチマーク: 1チップ順次 vs 全チップ非同期パイプライン ===
    printf("\n=== ベンチマーク (%u ページ, %.1fMHz) ===\n", groupPages, hz / 1e6);

    // 1チップだけを使った同期書き込み(比較のベースライン)。
    // groupPages分を1チップに収めるにはそのチップの物理ページも
    // groupPages個必要なので、同じ物理範囲(basePage/n始点)を使う
    {
        // paths[0]を独立したfdでもう一度開く(spidevは多重openを許すが、
        // devとsingleを同時に使わないので競合しない)
        std::vector<std::string> one = { paths[0] };
        W25NSpiFlashDevice single;
        if (!single.open(one, hz)) { fprintf(stderr, "単一チップopen失敗\n"); return 1; }
        // groupPages/64(=n)ブロック分を連続で使うので、その本数ぶん
        // 良品ブロックが連続する範囲をchip[0]内でスキャンする
        // (単一チップ内なのでブロック境界=論理オフセットそのまま)
        int block1 = find_good_run(single, 0, groupPages / 64, 1);
        if (block1 < 0) {
            fprintf(stderr, "単一チップ内に良品ブロックの連続範囲が見つからない\n");
            return 1;
        }
        printf("単一チップ用に使用ブロック=%d〜%d (良品確認済み)\n", block1, block1 + (int)(groupPages / 64) - 1);
        uint32_t off1 = (uint32_t)block1 * 64u * PAGE_SIZE;
        uint32_t len1 = groupPages * PAGE_SIZE;   /* = groupPages/64 ブロック分。単一チップ内で完結 */
        if (!single.erase(off1, len1)) {
            fprintf(stderr, "単一チップerase失敗\n");
            return 1;
        }
        double t0 = now_sec();
        if (!single.write(off1, wr.data(), len1)) { fprintf(stderr, "単一チップwrite失敗\n"); return 1; }
        double dt = now_sec() - t0;
        printf("sync (chip[0]のみ, %u ページ順次): %.3fs (%.1f pages/s, %.1f KB/s)\n",
               groupPages, dt, groupPages / dt, (len1 / 1024.0) / dt);
    }

    // 全チップへ非同期発行するパイプライン版(dev.write()そのもの)
    {
        if (!dev.erase(baseOff, groupBytes)) { fprintf(stderr, "erase失敗\n"); return 1; }
        double t0 = now_sec();
        if (!dev.write(baseOff, wr.data(), groupBytes)) { fprintf(stderr, "write失敗\n"); return 1; }
        double dt = now_sec() - t0;
        printf("pipelined (%zuチップ非同期, %u ページ): %.3fs (%.1f pages/s, %.1f KB/s)\n",
               n, groupPages, dt, groupPages / dt, (groupBytes / 1024.0) / dt);
    }

    printf("\n理想的にはpipelinedがsyncの約%zu倍(チップ数分)に近づくほど、\n"
           "Program時間(tPP)のオーバーラップが効いている\n", n);
    return 0;
}
