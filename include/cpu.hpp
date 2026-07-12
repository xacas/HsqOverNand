#ifndef CPU_HPP
#define CPU_HPP

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "nandInterface.hpp"
#include "ROM.hpp"

class CPU
{
    public:
        // devはこのCPUが占有するフラッシュデバイス(1デバイス=1CPU)
        CPU(const char* dev = "/dev/mtd0")
         : nand(dev)
        {
            //init ROM
            romAddr = 0;
            calcAddr = RAM;
            std::vector<ROM_t> rom;
            uint8_t* nand_internal = new uint8_t[ROM_SIZE];
            // 未使用領域は0xff(プログラム無し)にしておく(読み戻し検証のため)
            memset(nand_internal, 0xff, ROM_SIZE);
            rom_init(rom);
            for(size_t i = 0; i < rom.size(); i++)
            {
                uint64_t *pt = (uint64_t*)&nand_internal[i * 8];
                uint64_t word;
                word = ((uint64_t)rom[i].code << 61)
                        + ((uint64_t)rom[i].dest << 40)
                        + ((uint64_t)rom[i].from << 20)
                        + (uint64_t)rom[i].next;
                *pt = word;
            }
            rom_size = rom.size();
            if(rom_size * sizeof(uint64_t) > ROM_SIZE)
            {
                printf("rom_init overflow: %u words > ROM_SIZE\n", rom_size);
                exit(1);
            }
            nand.write(0, nand_internal, ROM_SIZE);

            // フラッシュ書込はANDのため、旧データが残っているとROMが壊れる。
            // 読み戻して一致しなければ起動を中止する
            uint8_t* verify = new uint8_t[ROM_SIZE];
            nand.read(0, verify, ROM_SIZE);
            if(memcmp(verify, nand_internal, ROM_SIZE) != 0)
            {
                printf("ROM verify FAILED: フラッシュに旧データが残っています。"
                       "nandsimを再ロードしてください(rmmod/modprobe = run.sh)\n");
                exit(1);
            }
            delete[] verify;
            delete[] nand_internal;

            // ウェアレベリング: 計算ページはラウンド毎に物理ページを進め、
            // 全容量を使い切ったときだけブロック消去する
            nandSize = nand.size();
            erase();
            printf("%s: wear-leveling nand=%u MB, %u pages/erase-cycle\n",
                   dev, nandSize >> 20, (nandSize - RAM) / PAGE_SIZ);
        };
        ROM_t fetch(void);
        void execute(ROM_t rom);
        uint32_t getRomSize(void){return rom_size;};
        uint32_t getCalcSize(void){return calcAddr;};
        uint32_t pc = 0;
        uint64_t out[LANES] = {};
        uint64_t inA[LANES] = {}, inB[LANES] = {};
        bool signal = false;
        bool is_jmp[LANES] = {};
    private:
        void erase(void);
        uint8_t read(uint32_t addr);
        void edit_buf(uint32_t addr, uint8_t value);
        void write(uint32_t addr);
        uint8_t page[PAGE_SIZ] = {}, rom[PAGE_SIZ] = {};
        uint32_t rom_size;
        uint32_t getinCnt = 0;	// GET_INのレーン判別カウンタ(CAP_INでリセット)
        uint32_t pageOffset = 0;	// 計算ページの現在の物理オフセット(RAM起点)
        uint32_t preOffset = 0;	// edit_bufのページ切替検出用(CPU毎に独立)
        uint32_t nandSize;
        // 計算領域(RAM以降)のROMアドレスを、ウェアレベリング先の物理アドレスへ変換
        uint32_t phys(uint32_t addr) { return (addr >= RAM) ? addr + pageOffset : addr; }
        nandInterface nand;
};
#endif
