#ifndef ROM_HPP
#define ROM_HPP

#include <stdint.h>
#include <vector>

#define ROM_SIZE 0xa0000
#define ROM_NUM ROM_SIZE/sizeof(ROM_t)
#define RAM ROM_SIZE
#define DIGIT 64
#define END (DIGIT - 1)
#define PAGE_SIZ 2048
#define MTD_SHADOW_SIZE 250000
// 1ページに載せる独立減算回路の数(並列レーン数)
#define LANES 2

enum opcode
{
    MOV,
    INV_MOV,
    CAP_IN,
    GET_IN,
    PRG_NAND,
    PUSH_OUT,
    RAISE,
    ERASE
};

const char opcode_str[8][10] = {
    "MOV",
    "INV_MOV",
    "CAP_IN",
    "GET_IN",
    "PRG_NAND",
    "PUSH_OUT",
    "RAISE",
    "ERASE"
};

#pragma pack(push, 1)

typedef struct _ROM_t
{
    opcode code : 3;
    uint64_t reserved : 1;
    uint64_t dest : 20;
    uint64_t from : 20;
    uint64_t next : 20;
}ROM_t;

#pragma pack(pop)

void rom_init(std::vector<ROM_t>& rom);
extern uint32_t romAddr;
extern uint32_t calcAddr;

#endif
