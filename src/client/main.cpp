#include <stdio.h>
#include <stdint.h>
#include "client.hpp"

static uint64_t rng = 0x0123456789abcdefULL;
static uint64_t xorshift64(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static int test(int64_t a, int64_t b)
{
	Request req = {a, b};
	Response resp = flashClient(req);
	int64_t expect = (int64_t)((uint64_t)b - (uint64_t)a);
	bool expect_jmp = (expect <= 0);
	if((resp.c == expect) && (resp.is_jmp == expect_jmp))
	{
		printf("[  OK  ] %ld = %ld - %ld, is_jmp = %d\n", resp.c, b, a, resp.is_jmp);
		return 0;
	}
	printf("[FAILED] %ld - %ld: c = %ld (expect %ld), is_jmp = %d (expect %d)\n",
		b, a, resp.c, expect, resp.is_jmp, expect_jmp);
	return 1;
}

int main(void)
{
	// 小さい値の全数
	for(int64_t i = -32; i < 32; i++)
	{
		for(int64_t j = -32; j < 32; j++)
		{
			if(test(i, j)) return 1;
		}
	}
	// 64bit境界値
	const int64_t edges[] = {
		0, 1, -1, 2, -2,
		INT64_MAX, INT64_MIN, INT64_MAX - 1, INT64_MIN + 1,
		0x100000000LL, -0x100000000LL, 0x7FFFFFFFLL, -0x80000000LL
	};
	const int nedges = sizeof(edges) / sizeof(edges[0]);
	for(int i = 0; i < nedges; i++)
	{
		for(int j = 0; j < nedges; j++)
		{
			if(test(edges[i], edges[j])) return 1;
		}
	}
	// ランダム64bit
	for(int k = 0; k < 512; k++)
	{
		if(test((int64_t)xorshift64(), (int64_t)xorshift64())) return 1;
	}
	printf("All tests passed.\n");
	return 0;
}
