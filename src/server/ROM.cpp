#include "ROM.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <deque>

uint32_t romAddr = 0;
uint32_t calcAddr = RAM;

static uint32_t incAddr()
{
	return romAddr += 0x08;
}

static uint32_t getCalcAddr()
{
	return calcAddr++;
}

class operation
{
public:
	operation(opcode op, uint32_t dest, uint32_t from)
	 : code({op, 0, dest, from, incAddr()}){}
	ROM_t code;
	// ROM_t型への暗黙の型変換演算子を追加
	operator ROM_t() const {
		return code;
	}
};

static void push_op(std::vector<ROM_t>& rom, opcode op, uint32_t dest, uint32_t from)
{
	rom.push_back(operation(op, dest, from));
}

namespace {

// セル(=フラッシュの1バイト)は「各ステップで書き込まれた値のAND」を蓄積する。
// MOVは正転、INV_MOVは反転書き込みなので、どのセルも両極性で読み出せる。
// 信号 = セル + 読み出し極性。
struct Sig
{
	int cell;
	bool inv;
};

static Sig NOT(Sig s)
{
	return {s.cell, !s.inv};
}

struct Write
{
	Sig src;
	int step;
};

struct Cell
{
	uint32_t addr;
	std::vector<Write> ins;
	int complete;	// この値のPRG_NANDステップ以降で確定 (-1 = 未スケジュール)
};

class Netlist
{
public:
	// GET_INでステップ1に値が入る入力セル
	int input()
	{
		cells.push_back({getCalcAddr(), {}, 1});
		return (int)cells.size() - 1;
	}
	int cell()
	{
		cells.push_back({getCalcAddr(), {}, -1});
		return (int)cells.size() - 1;
	}
	void wire(int dst, Sig src, bool inv = false)
	{
		cells[dst].ins.push_back({{src.cell, src.inv != inv}, -1});
	}
	uint32_t addrOf(Sig s)
	{
		assert(!s.inv);
		return cells[s.cell].addr;
	}

	// AND: 1つのセルに全入力を(ステップを分けて)書き込む
	Sig andOf(const std::vector<Sig>& xs)
	{
		assert(!xs.empty());
		if(xs.size() == 1)
		{
			return xs[0];
		}
		int t = cell();
		for(size_t i = 0; i < xs.size(); i++)
		{
			wire(t, xs[i]);
		}
		return {t, false};
	}
	// OR: ド・モルガン。セル値はNOR、反転極性で読めばOR
	Sig orOf(const std::vector<Sig>& xs)
	{
		assert(xs.size() >= 2);
		int t = cell();
		for(size_t i = 0; i < xs.size(); i++)
		{
			wire(t, NOT(xs[i]));
		}
		return {t, true};
	}
	// XOR: ~(a&b) & ~(~a&~b) を正極性セルとして返す
	Sig xorOf(Sig a, Sig b)
	{
		Sig hi = andOf({a, b});
		Sig lo = andOf({NOT(a), NOT(b)});
		return NOT(orOf({hi, lo}));
	}
	// 反転信号を正極性セルに実体化(PUSH_OUT/RAISE用)
	Sig buf(Sig s)
	{
		if(!s.inv)
		{
			return s;
		}
		int t = cell();
		wire(t, s);
		return {t, false};
	}

	// 各書き込みにステップを割り当てる。
	// 制約: ソースは確定済みの値しか読めない / 同一セルへの書き込みは1ステップ1回
	int schedule()
	{
		size_t n = cells.size();
		std::vector<std::vector<int>> readers(n);
		std::vector<int> pend(n, 0);
		std::deque<int> q;
		size_t done = 0;
		int maxStep = 1;

		for(size_t i = 0; i < n; i++)
		{
			pend[i] = (int)cells[i].ins.size();
			for(size_t j = 0; j < cells[i].ins.size(); j++)
			{
				readers[cells[i].ins[j].src.cell].push_back((int)i);
			}
			if(cells[i].complete == 1)
			{
				assert(cells[i].ins.empty());
				q.push_back((int)i);
			}
			else
			{
				assert(!cells[i].ins.empty());
			}
		}

		while(!q.empty())
		{
			int c = q.front();
			q.pop_front();
			done++;

			if(cells[c].complete < 0)
			{
				// ソースの確定が早い順に書き込むと深さが最小になる
				std::stable_sort(cells[c].ins.begin(), cells[c].ins.end(),
					[&](const Write& l, const Write& r)
					{
						return cells[l.src.cell].complete < cells[r.src.cell].complete;
					});
				int t = 1;
				for(size_t j = 0; j < cells[c].ins.size(); j++)
				{
					t = std::max(cells[cells[c].ins[j].src.cell].complete, t) + 1;
					cells[c].ins[j].step = t;
				}
				cells[c].complete = t;
				maxStep = std::max(maxStep, t);
			}
			for(size_t j = 0; j < readers[c].size(); j++)
			{
				if(--pend[readers[c][j]] == 0)
				{
					q.push_back(readers[c][j]);
				}
			}
		}
		assert(done == n && "combinational loop in netlist");
		return maxStep;
	}

	std::vector<Cell> cells;
};

// 1ラウンド = LANES回の独立な64bit減算 b - a を同一ページで並列実行。
// cpu.cppのread()は現在のページバッファしか参照しないため、
// 1ラウンドの全セル(全レーン分)は同一フラッシュページに収める必要がある。
// PRG_NANDはページ単位なので、全レーンがプログラムステップを共有できる。
static void build_lane(Netlist& net, int* ain, int* bin, Sig* sum, Sig* isJmp)
{
	static_assert(DIGIT % 16 == 0 && DIGIT / 16 <= 4, "hierarchy is 4bit x 4group x 4super");
	const int NGRP = DIGIT / 4;
	const int NSUP = NGRP / 4;

	for(int i = 0; i < DIGIT; i++)
	{
		ain[i] = net.input();
		bin[i] = net.input();
	}

	// 定数0: 同じ信号とその反転のAND
	Sig zero = net.andOf({{bin[0], false}, {bin[0], true}});

	// ビット毎の半加算: x = a^b, ボロー生成 g = a&~b, 伝播 p = ~(a^b)
	Sig g[DIGIT], p[DIGIT], x[DIGIT];
	for(int i = 0; i < DIGIT; i++)
	{
		Sig A = {ain[i], false}, B = {bin[i], false};
		x[i] = net.xorOf(A, B);
		g[i] = net.andOf({A, NOT(B)});
		p[i] = NOT(x[i]);
	}

	// 相対位置kへのボロー: bor_k = g_{k-1} | p_{k-1}g_{k-2} | ... | p_{k-1}..p_0*bin
	auto claBorrow = [&net](const Sig* G, const Sig* P, Sig borIn, int k) -> Sig
	{
		if(k == 0)
		{
			return borIn;
		}
		std::vector<Sig> terms;
		for(int j = k - 1; j >= 0; j--)
		{
			std::vector<Sig> f;
			for(int m = k - 1; m > j; m--)
			{
				f.push_back(P[m]);
			}
			f.push_back(G[j]);
			terms.push_back(net.andOf(f));
		}
		std::vector<Sig> f;
		for(int m = k - 1; m >= 0; m--)
		{
			f.push_back(P[m]);
		}
		f.push_back(borIn);
		terms.push_back(net.andOf(f));
		return net.orOf(terms);
	};
	// 4信号グループのグループ生成GG/伝播GP
	auto claGroup = [&net](const Sig* G, const Sig* P, Sig* gg, Sig* gp)
	{
		std::vector<Sig> terms;
		for(int j = 3; j >= 0; j--)
		{
			std::vector<Sig> f;
			for(int m = 3; m > j; m--)
			{
				f.push_back(P[m]);
			}
			f.push_back(G[j]);
			terms.push_back(net.andOf(f));
		}
		*gg = net.orOf(terms);
		*gp = net.andOf({P[3], P[2], P[1], P[0]});
	};

	// 第1階層: 4bitグループ(16個)のGG/GP
	Sig gg1[DIGIT / 4], gp1[DIGIT / 4];
	for(int k = 0; k < NGRP; k++)
	{
		claGroup(&g[4 * k], &p[4 * k], &gg1[k], &gp1[k]);
	}
	// 第2階層: スーパーグループ(4個)のGG/GP
	Sig gg2[DIGIT / 16], gp2[DIGIT / 16];
	for(int s = 0; s < NSUP; s++)
	{
		claGroup(&gg1[4 * s], &gp1[4 * s], &gg2[s], &gp2[s]);
	}
	// 第3階層: 各スーパーグループへのボロー(最下位のボロー入力は0)
	Sig bsup[DIGIT / 16];
	for(int s = 0; s < NSUP; s++)
	{
		bsup[s] = claBorrow(gg2, gp2, zero, s);
	}
	// 各4bitグループへのボロー
	Sig bgrp[DIGIT / 4];
	for(int s = 0; s < NSUP; s++)
	{
		for(int t = 0; t < 4; t++)
		{
			bgrp[4 * s + t] = claBorrow(&gg1[4 * s], &gp1[4 * s], bsup[s], t);
		}
	}
	// 各ビットへのボローと最終和 sum = x ^ bor
	sum[0] = x[0];
	for(int k = 0; k < NGRP; k++)
	{
		for(int u = 0; u < 4; u++)
		{
			int i = 4 * k + u;
			if(i == 0)
			{
				continue;
			}
			Sig bor = claBorrow(&g[4 * k], &p[4 * k], bgrp[k], u);
			sum[i] = net.xorOf(x[i], bor);
		}
	}

	// is_jmp: 結果が0または負(符号ビット=1)
	std::vector<Sig> accs;
	for(int k = 0; k < DIGIT; k += 8)
	{
		int acc = net.cell();
		for(int i = k; i < k + 8; i++)
		{
			net.wire(acc, sum[i], true);	// acc = AND(~sum) = 8bit全て0
		}
		accs.push_back({acc, false});
	}
	Sig allZero = net.andOf(accs);
	*isJmp = net.buf(net.orOf({allZero, sum[DIGIT - 1]}));
}

static void build_round(std::vector<ROM_t>& rom, int* stepsOut, uint32_t* cellsOut)
{
	calcAddr = ((calcAddr + PAGE_SIZ - 1) / PAGE_SIZ) * PAGE_SIZ;
	uint32_t pageBase = calcAddr;

	Netlist net;

	int ain[LANES][DIGIT], bin[LANES][DIGIT];
	Sig sum[LANES][DIGIT], isJmp[LANES];
	for(int l = 0; l < LANES; l++)
	{
		build_lane(net, ain[l], bin[l], sum[l], &isJmp[l]);
	}

	int steps = net.schedule();

	assert(calcAddr - pageBase <= PAGE_SIZ && "round does not fit in one flash page");

	// ステップ順にROMへ展開
	push_op(rom, CAP_IN, 0, 0);	// CAP_INはCPUのGET_INレーンカウンタをリセットする
	for(int l = 0; l < LANES; l++)
	{
		for(int i = 0; i < DIGIT; i++)
		{
			// GET_INはinA/inBを下位ビットから消費する(レーン順で64bitずつ)
			push_op(rom, GET_IN, net.cells[bin[l][i]].addr, net.cells[ain[l][i]].addr);
		}
	}
	push_op(rom, PRG_NAND, pageBase, 0);
	for(int s = 2; s <= steps; s++)
	{
		for(size_t c = 0; c < net.cells.size(); c++)
		{
			const Cell& cl = net.cells[c];
			for(size_t j = 0; j < cl.ins.size(); j++)
			{
				if(cl.ins[j].step == s)
				{
					push_op(rom, cl.ins[j].src.inv ? INV_MOV : MOV,
						cl.addr, net.cells[cl.ins[j].src.cell].addr);
				}
			}
		}
		push_op(rom, PRG_NAND, pageBase, 0);
	}
	// PUSH_OUT/RAISEのdestフィールドはレーン番号
	for(int l = 0; l < LANES; l++)
	{
		for(int i = 0; i < DIGIT; i++)
		{
			push_op(rom, PUSH_OUT, l, net.addrOf(sum[l][i]));
		}
	}
	for(int l = 0; l < LANES; l++)
	{
		// 最終レーンのRAISEがsignalを立てラウンド完了を通知する
		push_op(rom, RAISE, l, net.addrOf(isJmp[l]));
	}

	*stepsOut = steps;
	*cellsOut = calcAddr - pageBase;
}

} // namespace

void rom_init(std::vector<ROM_t>& rom)
{
	romAddr = 0;
	calcAddr = RAM;

	// ROMには1ラウンド分だけ置く。ラウンド毎の物理ページ移動(ウェアレベリング)は
	// CPUのページオフセットレジスタが行うため、ROM側は先頭ページ(RAM)相対の
	// アドレスで生成し、末尾から先頭へ巻き戻すだけでよい。
	// 消去は起動時と全容量を使い切ったときにCPUが直接行う。
	int steps = 0;
	uint32_t cellsPerRound = 0;
	build_round(rom, &steps, &cellsPerRound);
	assert(rom.size() * sizeof(ROM_t) <= ROM_SIZE);

	//ROM restart to zero
	rom.back().next = 0x00;

	printf("rom_init: DIGIT=%d LANES=%d ops/round=%zu steps/round=%d cells/round=%u rom=%zu bytes\n",
		DIGIT, LANES, rom.size(), steps, cellsPerRound, rom.size() * sizeof(ROM_t));
}
