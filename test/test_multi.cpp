// 複数CPU(複数フラッシュデバイス)の並列動作検証。
// NAND_STUBで4個のCPUを作り、(1)ラウンドロビンでCPU間の状態独立性、
// (2)4スレッド同時実行でスレッド安全性を確認する。
// スタブは1MBなので192ラウンド毎に消去→巻き戻しも交差する。
#include <stdio.h>
#include <stdint.h>
#include <thread>
#include "cpu.hpp"

uint8_t mtd_shadow[MTD_SHADOW_SIZE];
uint8_t *mtd_data = mtd_shadow;

#define NCPU 4

static uint64_t xs(uint64_t& s)
{
	s ^= s << 13;
	s ^= s >> 7;
	s ^= s << 17;
	return s;
}

// 1ラウンド実行して両レーンを参照実装と比較する
static void run_round(CPU* cpu, uint64_t& seed, long* total, long* failed)
{
	int64_t a0 = (int64_t)xs(seed), b0 = (int64_t)xs(seed);
	int64_t a1 = (int64_t)xs(seed), b1 = (int64_t)xs(seed);
	cpu->inA[0] = (uint64_t)a0;
	cpu->inB[0] = (uint64_t)b0;
	cpu->inA[1] = (uint64_t)a1;
	cpu->inB[1] = (uint64_t)b1;
	for(size_t k = 0; k < cpu->getRomSize(); k++)
	{
		cpu->execute(cpu->fetch());
		if(cpu->signal)
		{
			break;
		}
	}
	const int64_t as[2] = {a0, a1}, bs[2] = {b0, b1};
	for(int l = 0; l < LANES; l++)
	{
		int64_t expect = (int64_t)((uint64_t)bs[l] - (uint64_t)as[l]);
		(*total)++;
		if((int64_t)cpu->out[l] != expect || cpu->is_jmp[l] != (expect <= 0))
		{
			(*failed)++;
			if(*failed <= 10)
			{
				printf("[FAILED] lane%d b=%016lx a=%016lx c=%016lx (expect %016lx)\n",
					l, (uint64_t)bs[l], (uint64_t)as[l],
					(uint64_t)cpu->out[l], (uint64_t)expect);
			}
		}
	}
}

int main(void)
{
	CPU* cpus[NCPU];
	for(int i = 0; i < NCPU; i++)
	{
		cpus[i] = new CPU("/stub");
	}

	// (1) ラウンドロビン: ページバッファ/preOffset/pageOffsetがCPU毎に独立か
	long total = 0, failed = 0;
	uint64_t seeds[NCPU] = {0x1111, 0x2222, 0x3333, 0x4444};
	for(long k = 0; k < 500; k++)
	{
		for(int i = 0; i < NCPU; i++)
		{
			run_round(cpus[i], seeds[i], &total, &failed);
		}
	}
	printf("round-robin: total=%ld failed=%ld\n", total, failed);

	// (2) 4スレッド並列: 各スレッドが自分のCPUを専有して同時に回す
	long ts[NCPU] = {}, fs[NCPU] = {};
	std::thread th[NCPU];
	for(int i = 0; i < NCPU; i++)
	{
		th[i] = std::thread([&, i]{
			uint64_t s = 0xabcd0000 + i;
			for(long k = 0; k < 1500; k++)
			{
				run_round(cpus[i], s, &ts[i], &fs[i]);
			}
		});
	}
	for(int i = 0; i < NCPU; i++)
	{
		th[i].join();
		total += ts[i];
		failed += fs[i];
	}
	printf("parallel: total=%ld failed=%ld\n", total, failed);

	if(failed == 0)
	{
		printf("ALL %ld TESTS PASSED\n", total);
		return 0;
	}
	printf("%ld / %ld FAILED\n", failed, total);
	return 1;
}
