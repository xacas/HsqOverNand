#ifndef W25N_SPI_FLASH_DEV_HPP
#define W25N_SPI_FLASH_DEV_HPP

// W25N02KV(またはSPI-NAND互換チップ)をLinux spidev(RP1のハードウェア
// SPIペリフェラル)経由でMTDFlashDevice相当のバイトオフセットAPIで見せる
// ラッパー。1〜N個のチップを束ねて1つの論理NANDとして扱う。
// ドライバ本体は src/spi/w25n_spidev.c。
//
// 【アドレスマッピング】論理ページ p はチップ (p % N) の物理ページ (p / N)
// にラウンドロビンで割り当てる。同一論理ページが複数チップにまたがる
// ことはないので、cpu.cppの「1ラウンドの全セルは同一ページに収める」
// 制約([[nand-subleq-architecture]])はチップ数に関係なく成立する。
//
// 【非同期Program】write()は各ページのload+issue(Program Execute送信のみ、
// 完了を待たない)を発行してから、必要になった時点(同じチップへの次発行 /
// 全ページ処理後)でまとめてwaitする。複数ページが異なるチップに
// またがる場合、あるチップのProgram時間(tPP最大700us)を他チップへの
// コマンド送信でオーバーラップできる。1ページに収まる書き込み
// (SUBLEQのPRG_NAND等、同一ページへの依存したANDの積み重ねで原理的に
// パイプライン化できない)は従来どおり同期実行と等価になる。
//
// チップ間の物理バス排他はカーネルのspi_masterキューが自動で行うため、
// PIO版(W25NFlashDevice.hpp)のようなホスト側mutexは不要。
//
// 【eraseの粒度】ページ単位のラウンドロビンにより、論理アドレス空間上の
// 128KBブロックは複数チップにまたがる。そのためeraseは128KB単位ではなく
// 「128KB×チップ数」単位でしか呼べない(erase()実装のコメント参照)。

#include <string>
#include <vector>
#include <stdio.h>
#include <stdint.h>

#include "w25n_spidev.h"

class W25NSpiFlashDevice {
private:
    struct Chip {
        w25n_spi_dev_t* dev = nullptr;
        std::string path;
    };
    std::vector<Chip> chips;

    static bool jedecOk(w25n_spi_dev_t* d) {
        uint8_t id[3] = {};
        w25n_spi_jedec_id(d, id);
        return id[0] == 0xEF && id[1] == 0xAA && id[2] == 0x22;
    }

    size_t chipIndexOf(uint32_t page) const { return page % chips.size(); }
    uint32_t physPageOf(uint32_t page) const { return page / (uint32_t)chips.size(); }

public:
    ~W25NSpiFlashDevice() { close(); }

    // paths: 束ねるチップの/dev/spidevパス(1個以上)。同一物理バスの
    // 複数CSでも、別バスでも構わない(カーネルが直列化/並行化を判断する)
    bool open(const std::vector<std::string>& paths, uint32_t speedHz) {
        for (const auto& path : paths) {
            w25n_spi_dev_t* d = w25n_spi_open(path.c_str(), speedHz);
            if (!d) {
                fprintf(stderr, "w25nSpi(%s): openに失敗\n", path.c_str());
                close();
                return false;
            }
            if (!jedecOk(d)) {
                fprintf(stderr, "w25nSpi(%s): JEDEC IDがW25N02KVでない\n", path.c_str());
                w25n_spi_close(d);
                close();
                return false;
            }
            // 電源投入時は毎回保護状態に戻るので解除する。
            // ECCはセル=書込値のAND特性を使うため無効必須
            if (w25n_spi_unprotect(d) < 0 || w25n_spi_set_ecc(d, 0) < 0) {
                fprintf(stderr, "w25nSpi(%s): 保護解除/ECC無効化に失敗\n", path.c_str());
                w25n_spi_close(d);
                close();
                return false;
            }
            chips.push_back({d, path});
        }
        return !chips.empty();
    }

    void close() {
        for (auto& c : chips) {
            if (c.dev) w25n_spi_close(c.dev);
        }
        chips.clear();
    }

    bool isOpen() const { return !chips.empty(); }
    size_t chipCount() const { return chips.size(); }

    bool read(uint32_t offset, uint8_t* buffer, size_t length) {
        while (length > 0) {
            uint32_t page = offset / W25N_SPI_PAGE_SIZE;
            uint16_t col = offset % W25N_SPI_PAGE_SIZE;
            size_t n = W25N_SPI_PAGE_SIZE - col;
            if (n > length) n = length;
            Chip& c = chips[chipIndexOf(page)];
            uint32_t phys = physPageOf(page);
            if (w25n_spi_page_read(c.dev, phys) < 0 ||
                w25n_spi_read_buf(c.dev, col, buffer, n) < 0) {
                fprintf(stderr, "w25nSpi(%s): read失敗 page=%u\n", c.path.c_str(), phys);
                return false;
            }
            offset += n;
            buffer += n;
            length -= n;
        }
        return true;
    }

    bool write(uint32_t offset, const uint8_t* buffer, size_t length) {
        const size_t n = chips.size();
        std::vector<int64_t> pendingPage(n, -1);   /* -1 = そのチップに未waitの発行なし */

        auto waitChip = [&](size_t idx) -> bool {
            if (pendingPage[idx] < 0) return true;
            bool ok = w25n_spi_program_wait(chips[idx].dev, 100000, nullptr) >= 0;
            if (!ok) {
                fprintf(stderr, "w25nSpi(%s): program失敗 page=%lld\n",
                        chips[idx].path.c_str(), (long long)pendingPage[idx]);
            }
            pendingPage[idx] = -1;
            return ok;
        };

        bool ok = true;
        while (length > 0 && ok) {
            uint32_t page = offset / W25N_SPI_PAGE_SIZE;
            uint16_t col = offset % W25N_SPI_PAGE_SIZE;
            size_t chunk = W25N_SPI_PAGE_SIZE - col;
            if (chunk > length) chunk = length;
            size_t idx = chipIndexOf(page);
            uint32_t phys = physPageOf(page);

            // 同じチップへの前回発行がまだ完了待ちなら、ここで完了を待つ
            // (チップの内部バッファ/ステートマシンは同時に1書き込みしか
            // 受け付けられないため)。異なるチップならバスは空いているので
            // 待たずにそのままload+issueへ進む
            ok = waitChip(idx);
            if (ok) {
                if (w25n_spi_program_load(chips[idx].dev, col, buffer, chunk) < 0 ||
                    w25n_spi_program_issue(chips[idx].dev, phys) < 0) {
                    fprintf(stderr, "w25nSpi(%s): load/issue失敗 page=%u\n",
                            chips[idx].path.c_str(), phys);
                    ok = false;
                } else {
                    pendingPage[idx] = phys;
                }
            }

            offset += chunk;
            buffer += chunk;
            length -= chunk;
        }

        for (size_t i = 0; i < n; i++)
            ok = waitChip(i) && ok;
        return ok;
    }

    // 注意: ページはチップにラウンドロビン割当なので、論理アドレス空間上の
    // 128KBブロック(64ページ)は複数チップにまたがる。1物理ブロックと
    // 1対1対応するのは「64ページ × チップ数」ごとの論理グループ単位
    // (groupSize)であり、offset/lengthはその倍数でなければならない。
    // 誤って128KB単位のままだと一部チップの物理ブロックしか消去されず、
    // 消去し忘れた領域に対する後続のAND書き込みが壊れる(実際に一度この
    // バグを作り込んで気づいた)
    bool erase(uint32_t offset, uint32_t length) {
        const uint32_t groupSize = W25N_SPI_BLOCK_SIZE * (uint32_t)chips.size();
        if (offset % groupSize != 0 || length % groupSize != 0) {
            fprintf(stderr,
                    "w25nSpi: eraseは%uバイト境界のみ (128KB×チップ数%zu, offset=%x len=%x)\n",
                    groupSize, chips.size(), offset, length);
            return false;
        }
        for (uint32_t off = offset; off < offset + length; off += groupSize) {
            uint32_t firstPage = off / W25N_SPI_PAGE_SIZE;   /* グループ先頭 = chips[0]の担当 */
            uint32_t physBlockPage = physPageOf(firstPage);  /* 全チップ共通の物理ブロック番号 */
            for (auto& c : chips) {
                if (w25n_spi_block_erase(c.dev, physBlockPage) < 0) {
                    fprintf(stderr, "w25nSpi(%s): erase失敗 page=%u\n", c.path.c_str(), physBlockPage);
                    return false;
                }
            }
        }
        return true;
    }

    uint32_t size() const { return (uint32_t)(W25N_SPI_TOTAL_SIZE * chips.size()); }

    // chipIdx番目のチップの物理ブロックblockが工場出荷時不良マークされて
    // いるか。W25N02KVはスペアエリア(列オフセット2048)の先頭バイトが
    // 0xFF以外なら不良ブロックという規約。読み出し自体に失敗した場合も
    // 安全側で不良扱いにする
    bool isBadBlock(size_t chipIdx, uint32_t block) {
        Chip& c = chips[chipIdx];
        uint32_t page = block * W25N_SPI_BLOCK_PAGES;
        uint8_t marker = 0xFF;
        if (w25n_spi_page_read(c.dev, page) < 0 ||
            w25n_spi_read_buf(c.dev, (uint16_t)W25N_SPI_PAGE_SIZE, &marker, 1) < 0)
            return true;
        return marker != 0xFF;
    }

    // 論理グループgroup(= 64*チップ数ページ)に対応する物理ブロックが、
    // 全チップで良品か(1チップでも不良なら不良グループ扱い)
    bool isGroupGood(uint32_t group) {
        for (size_t i = 0; i < chips.size(); i++)
            if (isBadBlock(i, group))
                return false;
        return true;
    }

    // pathのチップが応答するか(JEDEC IDのみ確認)。自動検出用
    static bool probe(const std::string& path, uint32_t speedHz) {
        w25n_spi_dev_t* d = w25n_spi_open(path.c_str(), speedHz);
        if (!d) return false;
        bool ok = jedecOk(d);
        w25n_spi_close(d);
        return ok;
    }
};

#endif // W25N_SPI_FLASH_DEV_HPP
