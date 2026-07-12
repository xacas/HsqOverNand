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

#include "MTDFlashDevice.hpp"
#include "gpiolib.h"

#undef DEBUG
#define LED

class nandInterface {
    private:
        MTDFlashDevice mtd;
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
        nandInterface(const char* dev = "/dev/mtd0") : mtd(dev)
        {
            if (!mtd.open()) {
                exit(1);
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
            mtd.close();
        }
        bool erase(uint32_t offset, uint32_t length)
        {
            bool status = false;
            led(ledErase, DRIVE_HIGH);

            if(mtd.erase(offset, length))
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

            if (mtd.read(offset, buffer, length)) {
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
            if(mtd.write(offset, buffer, length))
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
            return mtd.getInfo().size;
        }
};

#endif //NAND_STUB

#endif //NAND_IF_HPP
