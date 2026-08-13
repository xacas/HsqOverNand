#ifndef NAND_IF_HPP
#define NAND_IF_HPP

#ifdef NAND_STUB

// ネットリスト検証用のインメモリNAND。プログラム動作(1→0のみ)を再現する。
#include <stdint.h>
#include <string.h>
#include <vector>

class nandInterface {
    private:
        std::vector<uint8_t> mem = std::vector<uint8_t>(1 << 20, 0xff);
    public:
        nandInterface(const char* = "") {}
        bool erase(uint32_t offset, uint32_t length)
        {
            memset(&mem[offset], 0xff, length);
            return true;
        }
        bool read(uint32_t offset, uint8_t* buffer, size_t length)
        {
            memcpy(buffer, &mem[offset], length);
            return true;
        }
        bool write(uint32_t offset, const uint8_t* buffer, size_t length)
        {
            for(size_t i = 0; i < length; i++)
            {
                mem[offset + i] &= buffer[i];
            }
            return true;
        }
        uint32_t size()
        {
            return (uint32_t)mem.size();
        }
};

#else //NAND_STUB

#include <string.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include "MTDFlashDevice.hpp"
#include "W25NFlashDevice.hpp"
#include "W25NSpiFlashDevice.hpp"
#include "gpiolib.h"

#undef DEBUG
#define LED

// カンマ区切りの/dev/spidevパス列を分割する ("spi:"バックエンド用)
static std::vector<std::string> nandInterface_splitCsv(const char* s)
{
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = s; ; p++) {
        if (*p == ',' || *p == '\0') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (*p == '\0') break;
        } else {
            cur += *p;
        }
    }
    return out;
}

// デバイス指定:
//   "/dev/mtdN"          MTD (nandsim等、カーネルドライバ経由)
//   "hat:u2"/"hat:u3"    SPI-NAND HAT ver.4のW25N02KVをRP1 PIOで直接制御
//   "spi:/dev/spidevB.C[,/dev/spidevB2.C2,...]"
//                        W25N02KV(等)をLinuxのspidev(HWのSPIペリフェラル)
//                        経由で直接制御。複数チップを束ねると非同期
//                        Program(W25NSpiFlashDevice参照)でスループットが
//                        上がる。速度は環境変数W25N_SPI_HZで指定(既定20MHz)
class nandInterface {
    private:
        MTDFlashDevice* mtd = nullptr;
        W25NFlashDevice* hat = nullptr;
        W25NSpiFlashDevice* spi = nullptr;
#ifdef LED
        bool ledOk = false;
        unsigned int ledRead = 2;
        unsigned int ledWrite = 3;
        unsigned int ledErase = 4;
#endif
        void led(unsigned int pin, GPIO_DRIVE_T drive)
        {
#ifdef LED
            if(ledOk)
            {
                gpio_set_drive(pin, drive);
            }
#endif
        }
    public:
        nandInterface(const char* dev = "/dev/mtd0")
        {
            if(strncmp(dev, "hat:", 4) == 0)
            {
                hat = new W25NFlashDevice();
                if(!hat->open(dev + 4))
                {
                    exit(1);
                }
                // 注意: LEDピン(GPIO2/3/4)はHATのU3バス(io3/clk)と衝突する
                // ため、HAT使用時はLED表示を行わない(ledOk=falseのまま)
                return;
            }
            if(strncmp(dev, "spi:", 4) == 0)
            {
                spi = new W25NSpiFlashDevice();
                uint32_t hz = 20000000;  /* 要実測調整。まず保守的な値で確認 */
                if(const char* e = getenv("W25N_SPI_HZ"))
                {
                    hz = (uint32_t)strtoul(e, nullptr, 10);
                }
                if(!spi->open(nandInterface_splitCsv(dev + 4), hz))
                {
                    exit(1);
                }
                // 標準SPI0/SPI1のピン(GPIO7-11 / 16-21)はLEDピン(GPIO2-4)と
                // 衝突しないため、HATと異なりLED表示を継続する
            }
            else
            {
                mtd = new MTDFlashDevice(dev);
                if (!mtd->open()) {
                    exit(1);
                }
            }
#ifdef LED
            // GPIO初期化は全デバイス(CPU)で共有する(LEDピンは共通の稼働表示)。
            // GPIOが無い環境(非Raspberry Pi)ではLED表示だけ諦めて続行する
            static bool gpioTried = false;
            static bool gpioAvail = false;
            if(!gpioTried){
                gpioTried = true;
                if(gpiolib_init() < 0 || gpiolib_mmap() < 0){
                    printf("gpiolib unavailable, running without LEDs\n");
                } else {
                    gpio_set_fsel(ledRead, GPIO_FSEL_OUTPUT);
                    gpio_set_pull(ledRead, PULL_NONE);
                    gpio_set_drive(ledRead, DRIVE_LOW);

                    gpio_set_fsel(ledWrite, GPIO_FSEL_OUTPUT);
                    gpio_set_pull(ledWrite, PULL_NONE);
                    gpio_set_drive(ledWrite, DRIVE_LOW);

                    gpio_set_fsel(ledErase, GPIO_FSEL_OUTPUT);
                    gpio_set_pull(ledErase, PULL_NONE);
                    gpio_set_drive(ledErase, DRIVE_LOW);
                    gpioAvail = true;
                }
            }
            ledOk = gpioAvail;
#endif
        }
        ~nandInterface()
        {
            if(hat)
            {
                hat->close();
                delete hat;
            }
            if(spi)
            {
                spi->close();
                delete spi;
            }
            if(mtd)
            {
                mtd->close();
                delete mtd;
            }
        }
        bool erase(uint32_t offset, uint32_t length)
        {
            bool status = false;
            led(ledErase, DRIVE_HIGH);

            if(hat ? hat->erase(offset, length) : spi ? spi->erase(offset, length) : mtd->erase(offset, length))
            {
#ifdef DEBUG
                std::cout << "Erase successful." << std::endl;
#endif
                status = true;
            }
            led(ledErase, DRIVE_LOW);
            return status;
        }
        bool read(uint32_t offset, uint8_t* buffer, size_t length)
        {
            bool status = false;
            led(ledRead, DRIVE_HIGH);

            if (hat ? hat->read(offset, buffer, length) : spi ? spi->read(offset, buffer, length) : mtd->read(offset, buffer, length)) {
#ifdef DEBUG
                std::cout << "Read successful. Data (hex):" << std::endl;
                for (size_t i = 0; i < length; i++) {
                    printf("%02X ", buffer[i]);
                    if ((i + 1) % 16 == 0) {
                        printf("\n");
                    }
                }
                printf("\n");
#endif
                status = true;
            }
            led(ledRead, DRIVE_LOW);
            return status;
        }
        bool write(uint32_t offset, const uint8_t* buffer, size_t length)
        {
            bool status = false;
            led(ledWrite, DRIVE_HIGH);
#ifdef DEBUG
            std::cout << "Writing " << length << " bytes..." << std::endl;
#endif
            if(hat ? hat->write(offset, buffer, length) : spi ? spi->write(offset, buffer, length) : mtd->write(offset, buffer, length))
            {
#ifdef DEBUG
                std::cout << "Write successful." << std::endl;
#endif
                status = true;
            }
            led(ledWrite, DRIVE_LOW);
            return status;
        }
        uint32_t size()
        {
            return hat ? hat->size() : spi ? spi->size() : mtd->getInfo().size;
        }
};

#endif //NAND_STUB

#endif //NAND_IF_HPP
