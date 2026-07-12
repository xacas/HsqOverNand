// 実機を使わないネットリスト検証(2レーン並列版)。
// 実物の ROM.cpp / cpu.cpp を NAND_STUB(インメモリNAND)でそのまま実行し、
// 両レーンの 64bit減算 b - a と is_jmp(結果<=0) を参照実装と比較する。
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include "cpu.hpp"

uint8_t mtd_shadow[MTD_SHADOW_SIZE];
uint8_t *mtd_data = mtd_shadow;

static CPU cpu;
static long total = 0, failed = 0;

static void run_round(void)
{
	for(size_t k = 0; k < cpu.getRomSize(); k++)
	{
		cpu.execute(cpu.fetch());
		if(cpu.signal)
		{
			break;
		}
	}
}

// 両レーンに独立な入力を与えて同時に検証する
static void check2(int64_t a0, int64_t b0, int64_t a1, int64_t b1)
{
	cpu.inA[0] = (uint64_t)a0;
	cpu.inB[0] = (uint64_t)b0;
	cpu.inA[1] = (uint64_t)a1;
	cpu.inB[1] = (uint64_t)b1;
	run_round();

	const int64_t as[LANES] = {a0, a1}, bs[LANES] = {b0, b1};
	for(int l = 0; l < LANES; l++)
	{
		int64_t c = (int64_t)cpu.out[l];
		bool jmp = cpu.is_jmp[l];
		int64_t expect = (int64_t)((uint64_t)bs[l] - (uint64_t)as[l]);
		bool expect_jmp = (expect <= 0);
		total++;
		if(c != expect || jmp != expect_jmp)
		{
			failed++;
			if(failed <= 20)
			{
				printf("[FAILED] lane%d b=%016lx a=%016lx: c=%016lx (expect %016lx) jmp=%d (expect %d)\n",
					l, (uint64_t)bs[l], (uint64_t)as[l], (uint64_t)c, (uint64_t)expect, jmp, expect_jmp);
			}
		}
	}
}

static uint64_t rng = 0x0123456789abcdefULL;
static uint64_t xorshift64(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

int main(void)
{
	// 小さい値の全数 (レーン1には逆順の組を与えて独立性も確認)
	for(int64_t i = -48; i < 48; i++)
	{
		for(int64_t j = -48; j < 48; j++)
		{
			check2(i, j, j, i);
		}
	}
	printf("exhaustive small: total=%ld failed=%ld\n", total, failed);

	// 境界値の全組み合わせ
	const int64_t edges[] = {
		0, 1, -1, 2, -2, 3, -3,
		INT64_MAX, INT64_MIN, INT64_MAX - 1, INT64_MIN + 1,
		0x7FFFFFFFLL, 0x80000000LL, -0x80000000LL,
		0x100000000LL, -0x100000000LL,
		0x5555555555555555LL, (int64_t)0xAAAAAAAAAAAAAAAAULL,
		0x0101010101010101LL, 0x1000000000000000LL
	};
	const int n = sizeof(edges) / sizeof(edges[0]);
	for(int i = 0; i < n; i++)
	{
		for(int j = 0; j < n; j++)
		{
			check2(edges[i], edges[j],
				edges[(i + 7) % n], edges[(j + 13) % n]);
		}
	}
	printf("edges: total=%ld failed=%ld\n", total, failed);

	// ランダム64bit
	for(int k = 0; k < 5000; k++)
	{
		check2((int64_t)xorshift64(), (int64_t)xorshift64(),
			(int64_t)xorshift64(), (int64_t)xorshift64());
	}
	// キャリー/ボローが長距離伝播しやすいパターン
	for(int k = 0; k < 2000; k++)
	{
		uint64_t v = xorshift64(), w = xorshift64();
		check2((int64_t)v, (int64_t)(v + (k % 3) - 1),
			(int64_t)w, (int64_t)(w + ((k + 1) % 3) - 1));
	}
	// 片レーンのみ使用(ダミーレーン)のケース
	for(int k = 0; k < 500; k++)
	{
		check2((int64_t)xorshift64(), (int64_t)xorshift64(), 0, 0);
	}
	printf("random: total=%ld failed=%ld\n", total, failed);

	if(failed == 0)
	{
		printf("ALL %ld TESTS PASSED\n", total);
		return 0;
	}
	printf("%ld / %ld FAILED\n", failed, total);
	return 1;
}
