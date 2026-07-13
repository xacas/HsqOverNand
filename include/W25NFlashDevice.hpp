#ifndef W25N_FLASH_DEV_HPP
#define W25N_FLASH_DEV_HPP

// SPI-NAND HAT ver.4 (W25N02KV×2、RP1 PIO直接制御) をMTDFlashDevice相当の
// バイトオフセットAPIで見せるラッパー。ドライバ本体は src/qspi/w25n_pio.c。
//
// 【チップ間ロック】U2/U3はバス独立だが、x4送信(Quad Data Load)のワイドOUTは
// 他チップのCLK/DIの出力ラッチも上書きするため、x4送信中に他チップへ
// コマンドを流してはならない (w25n_pio.c冒頭の排他制約参照)。そこで
// プロセス内共有のshared_timed_mutexで x4送信=排他 / それ以外=共有 を取る。
// x1のみの操作(読み出し・消去・Program Execute・BUSYポーリング)は
// チップ間で並行できるので、書き込みのデータロード相だけが直列化される。

#include <mutex>
#include <shared_mutex>
#include <string>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

#include "w25n_pio.h"

class W25NFlashDevice {
private:
    w25n_dev_t* dev = nullptr;
    std::string name;

    // 実測の信頼動作域はclkdiv>=8。マージンを取って10 (SPI 10MHz)
    static constexpr float CLKDIV = 10.0f;

    static std::shared_timed_mutex& busMu() {
        static std::shared_timed_mutex mu;
        return mu;
    }

    static bool jedecOk(w25n_dev_t* d) {
        uint8_t id[3] = {};
        w25n_jedec_id(d, id);
        return id[0] == 0xEF && id[1] == 0xAA && id[2] == 0x22;
    }

public:
    ~W25NFlashDevice() { close(); }

    // chip: "u2" または "u3"
    bool open(const std::string& chip) {
        name = chip;
        const w25n_pins_t pins = (chip == "u3") ? W25N_U3_PINS : W25N_U2_PINS;
        dev = w25n_open(&pins, CLKDIV);
        if (!dev) {
            fprintf(stderr, "w25n(%s): openに失敗\n", chip.c_str());
            return false;
        }
        if (!jedecOk(dev)) {
            fprintf(stderr, "w25n(%s): JEDEC IDがW25N02KVでない\n", chip.c_str());
            close();
            return false;
        }
        // 電源投入時は毎回保護状態に戻るので解除する。
        // ECCはセル=書込値のAND特性を使うため無効必須
        if (w25n_unprotect(dev) < 0 || w25n_set_ecc(dev, 0) < 0) {
            fprintf(stderr, "w25n(%s): 保護解除/ECC無効化に失敗\n", chip.c_str());
            close();
            return false;
        }
        return true;
    }

    void close() {
        if (dev) {
            w25n_close(dev);
            dev = nullptr;
        }
    }

    bool isOpen() const { return dev != nullptr; }

    bool read(uint32_t offset, uint8_t* buffer, size_t length) {
        std::shared_lock<std::shared_timed_mutex> lk(busMu());
        while (length > 0) {
            uint32_t page = offset / W25N_PAGE_SIZE;
            uint16_t col = offset % W25N_PAGE_SIZE;
            size_t n = W25N_PAGE_SIZE - col;
            if (n > length) n = length;
            if (w25n_page_read(dev, page) < 0 ||
                w25n_read_buf(dev, col, buffer, n) < 0) {
                fprintf(stderr, "w25n(%s): read失敗 page=%u\n", name.c_str(), page);
                return false;
            }
            offset += n;
            buffer += n;
            length -= n;
        }
        return true;
    }

    bool write(uint32_t offset, const uint8_t* buffer, size_t length) {
        while (length > 0) {
            uint32_t page = offset / W25N_PAGE_SIZE;
            uint16_t col = offset % W25N_PAGE_SIZE;
            size_t n = W25N_PAGE_SIZE - col;
            if (n > length) n = length;
            {
                // データロード相(x4ワイドOUT)は他チップと排他
                std::unique_lock<std::shared_timed_mutex> lk(busMu());
                if (w25n_program_begin(dev, col, buffer, n) < 0) {
                    fprintf(stderr, "w25n(%s): load失敗 page=%u\n", name.c_str(), page);
                    return false;
                }
            }
            {
                // Program Execute+完了待ちはx1のみなので共有でよい
                std::shared_lock<std::shared_timed_mutex> lk(busMu());
                if (w25n_program_commit(dev, page) < 0) {
                    fprintf(stderr, "w25n(%s): program失敗 page=%u\n", name.c_str(), page);
                    return false;
                }
            }
            offset += n;
            buffer += n;
            length -= n;
        }
        return true;
    }

    bool erase(uint32_t offset, uint32_t length) {
        if (offset % W25N_BLOCK_SIZE != 0 || length % W25N_BLOCK_SIZE != 0) {
            fprintf(stderr, "w25n(%s): eraseは128KBブロック境界のみ (offset=%x len=%x)\n",
                    name.c_str(), offset, length);
            return false;
        }
        std::shared_lock<std::shared_timed_mutex> lk(busMu());
        for (uint32_t off = offset; off < offset + length; off += W25N_BLOCK_SIZE) {
            if (w25n_block_erase(dev, off / W25N_PAGE_SIZE) < 0) {
                fprintf(stderr, "w25n(%s): erase失敗 block=%u\n",
                        name.c_str(), off / W25N_BLOCK_SIZE);
                return false;
            }
        }
        return true;
    }

    uint32_t size() const { return W25N_TOTAL_SIZE; }

    // HATが接続されているか(U2のJEDEC IDで代表プローブ)。
    // /dev/pio0が無い環境(非RPi5)では即false
    static bool probe() {
        if (access("/dev/pio0", F_OK) != 0) {
            return false;
        }
        const w25n_pins_t pins = W25N_U2_PINS;
        w25n_dev_t* d = w25n_open(&pins, CLKDIV);
        if (!d) {
            return false;
        }
        bool ok = jedecOk(d);
        w25n_close(d);
        return ok;
    }
};

#endif // W25N_FLASH_DEV_HPP
