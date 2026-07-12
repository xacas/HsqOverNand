#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "cpu.hpp"

#define DEBUG_MODE 0

#if DEBUG_MODE
#define DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#endif

#undef LATENCY
#ifdef LATENCY
#define U_SLEEP(...) usleep(__VA_ARGS__)
#else
#define U_SLEEP(...)
#endif

bool majorityVote(uint8_t bits)
{
    uint8_t count = 0;
    for(unsigned int i = 0; i < 8; i++, bits >>= 1)
    {
        if(bits & 0x01)
        {
            count++;
        }
    }
    return (count > 3);
}

void CPU::erase(void)
{
    DEBUG_PRINT("erase nand flash...\n");
    // 計算領域の全ブロックを一括消去する(1周で各ブロック1回だけの消去になる)
    if(!nand.erase(RAM, nandSize - RAM))
    {
        printf("erase FAILED: calc area %x-%x\n", (uint32_t)RAM, nandSize);
    }
    extern uint8_t *mtd_data;
    memset(mtd_data, 0xff, MTD_SHADOW_SIZE);
    pageOffset = 0;
    memset(page, 0xff, PAGE_SIZ);
    U_SLEEP(10000);
}

uint8_t CPU::read(uint32_t addr)
{
    uint8_t data = page[addr % PAGE_SIZ];
    DEBUG_PRINT("%02x = read(%05x)\n", data, addr);
    return data;
}

void CPU::edit_buf(uint32_t addr, uint8_t value)
{
    DEBUG_PRINT("edit_buf %05x, %02x\n", addr, value);
    uint32_t offset = (phys(addr) / PAGE_SIZ) * PAGE_SIZ;
    static uint32_t preOffset = 0;
    if(offset != preOffset)
    {
        DEBUG_PRINT("offset = %x, preOffset = %x\n", offset, preOffset);
        DEBUG_PRINT("Issue page buffer clear!\n");
        memset(page, 0xff, PAGE_SIZ);
    }

    page[addr % PAGE_SIZ] = value;

    preOffset = offset;
}

void CPU::write(uint32_t addr)
{
    uint32_t offset = (phys(addr) / PAGE_SIZ) * PAGE_SIZ;
    DEBUG_PRINT("write offset %07x\n", offset);
    nand.write(offset, page, PAGE_SIZ);
    U_SLEEP(40);
    nand.read(offset, page, PAGE_SIZ);
    U_SLEEP(15);
    // GUI用シャドウ: プログラム済みページの実内容をリング状に写す
    extern uint8_t *mtd_data;
    memcpy(&mtd_data[((offset - RAM) / PAGE_SIZ % (MTD_SHADOW_SIZE / PAGE_SIZ)) * PAGE_SIZ],
           page, PAGE_SIZ);
}

ROM_t CPU::fetch(void)
{
    uint64_t inst;
    ROM_t ret_inst = {};

    if((pc % PAGE_SIZ) == 0)
    {
        nand.read(pc, rom, PAGE_SIZ);
    }
    memcpy(&inst, &rom[pc % PAGE_SIZ], 8);
    DEBUG_PRINT("inst = %lx\n", inst);

    ret_inst.code = (opcode)(inst >> 61);
    ret_inst.dest = (inst >> 40) & 0xfffff;
    ret_inst.from = (inst >> 20) & 0xfffff;
    ret_inst.next = (inst >>  0) & 0xfffff;

#if DEBUG_MODE
    DEBUG_PRINT("pc = %07x,fetch code = %s, dest = %05x, from = %05x, next = %07x\n",
        pc, opcode_str[ret_inst.code], (uint32_t)ret_inst.dest,
        (uint32_t)ret_inst.from, (uint32_t)ret_inst.next);
#endif
    return (ROM_t)ret_inst;
}

void CPU::execute(ROM_t rom)
{
    //|opcode|reserved|from|dest|next|
    uint8_t code = rom.code;
    uint32_t from = rom.from;
    uint32_t dest = rom.dest;
    uint32_t next = rom.next;

    signal = false;

    switch (code)
    {
    case MOV:
        edit_buf(dest, read(from));
        break;
    case INV_MOV:
        edit_buf(dest, ~read(from));
        break;
    case CAP_IN:
        DEBUG_PRINT("Capture input bits\n");
        getinCnt = 0;
        break;
    case GET_IN:
    {
        // ROMはレーン順に64bitずつGET_INを並べる
        uint32_t lane = getinCnt / DIGIT;
        if (lane >= LANES) lane = LANES - 1;
        getinCnt++;
        edit_buf(from, ((inA[lane] & 0x0001)) ? 0xff : 0x00);
        inA[lane] >>= 1;
        edit_buf(dest, ((inB[lane] & 0x0001)) ? 0xff : 0x00);
        inB[lane] >>= 1;
        break;
    }
    case PRG_NAND:
        write(dest);
        break;
    case PUSH_OUT:
        // destフィールド = レーン番号
        out[dest % LANES] >>= 1;
        out[dest % LANES] |= ((uint64_t)majorityVote(read(from)) << (DIGIT - 1));
        break;
    case RAISE:
        DEBUG_PRINT("RAISE signal. lane = %u, out = %016lx\n", dest, out[dest % LANES]);
        is_jmp[dest % LANES] = majorityVote(read(from));
        // 最終レーンのRAISEでラウンド完了
        if (dest % LANES == LANES - 1)
        {
            signal = true;
            // ウェアレベリング: 次ラウンドは次の物理ページを使い、
            // 全容量を使い切ったときだけ消去する
            pageOffset += PAGE_SIZ;
            if (RAM + pageOffset + PAGE_SIZ > nandSize)
            {
                erase();
            }
        }
        break;
    case ERASE:
        DEBUG_PRINT("erase RAM Block\n...");
        erase();
        break;
    default:
        DEBUG_PRINT("unexpected error !\n");
        break;
    }

    pc = next;

    return;
}
