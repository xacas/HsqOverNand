// SPI-NAND HAT ver.4 実チップでのCPU統合検証 (sudo不要、/dev/pio0のみ必要)。
// - 1Gb境界(ページ65536)をまたぐアドレッシングの直接検証
// - U2/U3それぞれにCPUを構築し、2スレッド並行で64bit減算 b-a を照合
//   (x4送信のチップ間排他ロックとRP1ドライバの並行動作の検証を兼ねる)
// 実チップは1ラウンドが遅い(数百ms)ため、ラウンド数は絞ってある。
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <thread>
#include "cpu.hpp"
#include "W25NFlashDevice.hpp"

uint8_t mtd_shadow[MTD_SHADOW_SIZE];
uint8_t *mtd_data = mtd_shadow;

// ページアドレスが17bit(>1Gb)まで正しく届くかを直接確認する。
// CPU構築前に行う(この後のCPU初期化が計算領域全消去で痕跡を消す)
static bool check_boundary(const char* chip)
{
	W25NFlashDevice f;
	if(!f.open(chip))
	{
		return false;
	}
	// 1Gb境界直後のブロック(ページ65536 = ブロック1024)
	const uint32_t off = 65536u * W25N_PAGE_SIZE;
	uint8_t wr[W25N_PAGE_SIZE], rd[W25N_PAGE_SIZE];
	for(size_t i = 0; i < sizeof(wr); i++)
	{
		wr[i] = (uint8_t)(i * 31 + 7);
	}
	bool ok = f.erase(off, W25N_BLOCK_SIZE) &&
	          f.write(off, wr, sizeof(wr)) &&
	          f.read(off, rd, sizeof(rd)) &&
	          memcmp(wr, rd, sizeof(rd)) == 0;
	// ついでにページ0との独立性(境界の折り返しが無いこと)を確認
	if(ok)
	{
		uint8_t rd0[16];
		ok = f.read(0, rd0, sizeof(rd0)) &&
		     memcmp(rd0, rd, sizeof(rd0)) != 0;
	}
	f.close();
	printf("%s: 1Gb境界アドレッシング %s\n", chip, ok ? "OK" : "NG");
	return ok;
}

struct Result
{
	long total = 0, failed = 0;
};

static void run_round(CPU& cpu)
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

static void check2(const char* name, CPU& cpu, Result& r,
	int64_t a0, int64_t b0, int64_t a1, int64_t b1)
{
	cpu.inA[0] = (uint64_t)a0;
	cpu.inB[0] = (uint64_t)b0;
	cpu.inA[1] = (uint64_t)a1;
	cpu.inB[1] = (uint64_t)b1;
	run_round(cpu);

	const int64_t as[LANES] = {a0, a1}, bs[LANES] = {b0, b1};
	for(int l = 0; l < LANES; l++)
	{
		int64_t c = (int64_t)cpu.out[l];
		bool jmp = cpu.is_jmp[l];
		int64_t expect = (int64_t)((uint64_t)bs[l] - (uint64_t)as[l]);
		bool expect_jmp = (expect <= 0);
		r.total++;
		if(c != expect || jmp != expect_jmp)
		{
			r.failed++;
			printf("[FAILED] %s lane%d b=%016lx a=%016lx: c=%016lx (expect %016lx) jmp=%d (expect %d)\n",
				name, l, (uint64_t)bs[l], (uint64_t)as[l],
				(uint64_t)c, (uint64_t)expect, jmp, expect_jmp);
		}
	}
}

static void worker(const char* name, CPU* cpu, Result* r, uint64_t rng)
{
	// 小さい値・境界値
	check2(name, *cpu, *r, 3, 5, 5, 3);
	check2(name, *cpu, *r, -7, 2, 0, 0);
	check2(name, *cpu, *r, INT64_MAX, INT64_MIN, -1, 1);
	check2(name, *cpu, *r, INT64_MIN, INT64_MAX, 1, -1);
	check2(name, *cpu, *r, 0x5555555555555555LL, (int64_t)0xAAAAAAAAAAAAAAAAULL,
		(int64_t)0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555LL);
	// ランダム + キャリー長距離伝播
	for(int k = 0; k < 5; k++)
	{
		rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
		uint64_t v = rng;
		rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
		check2(name, *cpu, *r, (int64_t)v, (int64_t)rng,
			(int64_t)v, (int64_t)(v + (k % 3) - 1));
	}
}

int main(void)
{
	if(!check_boundary("u2") || !check_boundary("u3"))
	{
		return 1;
	}

	// CPU構築はROM生成のグローバル状態(romAddr/calcAddr)のため必ず逐次。
	// 計算領域の全消去を含むため数秒〜十数秒かかる
	printf("CPU(u2)を初期化中...\n");
	CPU* u2 = new CPU("hat:u2");
	printf("CPU(u3)を初期化中...\n");
	CPU* u3 = new CPU("hat:u3");

	// 2チップ並行実行 (x4送信の排他ロックの検証を兼ねる)
	Result r2, r3;
	std::thread t2(worker, "U2", u2, &r2, 0x0123456789abcdefULL);
	std::thread t3(worker, "U3", u3, &r3, 0xfedcba9876543210ULL);
	t2.join();
	t3.join();

	printf("U2: total=%ld failed=%ld\n", r2.total, r2.failed);
	printf("U3: total=%ld failed=%ld\n", r3.total, r3.failed);
	delete u2;
	delete u3;

	if(r2.failed == 0 && r3.failed == 0 && r2.total > 0 && r3.total > 0)
	{
		printf("ALL %ld TESTS PASSED\n", r2.total + r3.total);
		return 0;
	}
	printf("FAILED\n");
	return 1;
}
