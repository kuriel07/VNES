#include "stdafx.h"
#include "defs.h"

typedef struct nes_mmc1 {
	uint8 cr;
	uint8 ch0;
	uint8 ch1;
	uint8 prg;
	uint8 cr_shift;
	uint8 ch0_shift;
	uint8 ch1_shift;
	uint8 prg_shift;
	unsigned bank_table[20];
} nes_mmc1;

typedef struct nes_mapper {
	uint8* rom;			//program rom
	int size;
	uint8* chrom;		//character rom
	int chsize;
	void* payload;
	void (*read)(void * payload, uint16 address, uint8 data);
	void (*write)(void* payload, uint16 address, uint8 data);
} nes_mapper;

#define PPU_CR1          0x2000
#define PPU_CR2         0x2001
#define PPU_SR          0x2002
#define PPU_SPR_ADDR    0x2003
#define PPU_SPR_DATA    0x2004
#define PPU_SCR_OFFSET  0x2005
#define PPU_MEM_ADDR       0x2006
#define PPU_MEM_DATA        0x2007
extern void ppu_dma_write(uint8* data, size_t size);
extern void ppu_set_ram(uint16 address, uint8* data, size_t size);
extern void ppu_set_cr1(uint8 data);
extern uchar ppu_get_cr1();
extern void ppu_set_cr2(uint8 data);
extern uchar ppu_get_cr2();
extern void ppu_set_sr(uint8 data);
extern uchar ppu_get_sr();
extern void ppu_set_scroll(uint8 data);
extern uchar ppu_get_scroll();
extern void ppu_set_spr_addr(uint8 data);
extern uchar ppu_get_spr_addr();
extern void ppu_set_mem_addr(uint8 data);
extern uchar ppu_get_mem_addr();
extern void ppu_set_spr_data(uint8 data);
extern uchar ppu_get_spr_data();
extern void ppu_set_mem_data(uint8 data);
extern uchar ppu_get_mem_data();
extern void ppu_init(uint8 config);
extern void ppu_render(uchar* output);
extern void ppu_set_vblank(uchar flag);
extern uchar ppu_get_vblank();

#define SR_FLAG_N			0x80
#define SR_FLAG_V			0x40
#define SR_FLAG_B			0x10
#define SR_FLAG_D			0x08
#define SR_FLAG_I			0x04
#define SR_FLAG_Z			0x02
#define SR_FLAG_C			0x01

uint16 _pc = 0;				//program counter
uchar _acc = 0;				//accumulator
uchar _x = 0;				//x register
uchar _y = 0;				//y register
uchar _sr = 0;			//status register
uchar _sp = 0xfd;				//stack pointer

uchar _sram[65536];			//nes sram
uchar* _stack = _sram + 0x100;
nes_mapper _mmc = {
	NULL, 0, NULL, NULL
};



__forceinline uchar core_orl(uchar a, uchar operand) {
	//check D flag for BCD operation, C flag for carry operation, set C if needed
	register uint16 ret;
	ret = a | operand;
	return ret;
}

__forceinline uchar core_and(uchar a, uchar operand) {
	//check D flag for BCD operation, C flag for carry operation, set C if needed
	register uint16 ret;
	ret = a & operand;
	return ret;
}

__forceinline uchar core_xor(uchar a, uchar operand) {
	//check D flag for BCD operation, C flag for carry operation, set C if needed
	register uint16 ret;
	ret = a ^ operand;
	return ret;
}

__forceinline uchar core_lda(uchar a, uchar operand) {
	//check D flag for BCD operation, C flag for carry operation, set C if needed
	register uint16 ret;
	ret = operand;
	return ret;
}

__forceinline uchar core_asl(uchar a, uchar l) {
	register uint16 ret;
	ret = (uint16)a << l;
	if (ret & 0x100) _sr |= SR_FLAG_C;
	else _sr &= ~SR_FLAG_C;
	return ret;
}

__forceinline uchar core_lsr(uchar a, uchar l) {
	register uint16 ret;
	if (a & 0x01) _sr |= SR_FLAG_C;
	else _sr &= ~SR_FLAG_C;
	ret = (uint16)a >> l;
	return ret;
}

__forceinline uchar core_rol(uchar a, uchar l) {
	register uint16 ret;
	ret = (uint16)a << l;
	ret |= (_sr & SR_FLAG_C);
	if (ret & 0x100) _sr |= SR_FLAG_C;
	else _sr &= ~SR_FLAG_C;
	return ret;
}

__forceinline uchar core_ror(uchar a, uchar l) {
	register uint16 ret;
	register uint8 csr = (_sr & SR_FLAG_C);
	if (a & 0x01) _sr |= SR_FLAG_C;
	else _sr &= ~SR_FLAG_C;
	ret = (uint16)a >> l;
	if (csr) ret |= 0x80;
	return ret;
}

#define core_cmp(a, operand, psr) {	\
	if (a == operand) {	\
		psr |= SR_FLAG_Z;		\
		psr |= SR_FLAG_C;		\
		psr &= ~SR_FLAG_N;	\
	}	else if (a > operand) {		\
		psr &= ~SR_FLAG_Z;		\
		psr |= SR_FLAG_C;		\
		if((a- operand) & 0x80) psr |= SR_FLAG_N;		\
		else psr &= ~SR_FLAG_N;		\
	} else if (a < operand) {		\
		psr &= ~SR_FLAG_Z;		\
		psr &= ~SR_FLAG_C;	\
		if ((a - operand) & 0x80) psr |= SR_FLAG_N;		\
		else psr &= ~SR_FLAG_N;	\
	}	\
}

__forceinline uchar add_is_overflow(int8 lhs, int8 rhs) {
	if (lhs > 0 && rhs > 0 && (rhs > (INT8_MAX - lhs))) return 1;
	if (lhs < 0 && rhs < 0 && (lhs < (INT8_MIN - rhs))) return 1;
	return 0;
}

__forceinline uchar sub_is_overflow(int8 lhs, int8 rhs) {
	register int8 diff = lhs - rhs;
	//printf("diff:%x, lhs:%x, rhs:%x\n", diff, lhs, rhs);
	if (rhs >= 0 && diff > lhs)return 1;
	if (rhs < 0 && diff < lhs)return 1;
	//printf("return 0;\n");
	return 0;
}

__forceinline uchar core_add(uchar a, uchar operand, uchar* sr) {
	//check D flag for BCD operation, C flag for carry operation, set C if needed
	uchar nb = 0;
	register uchar bcr = 0;
	register uint16 ret;
	if (add_is_overflow(a, operand)) sr[0] |= SR_FLAG_V;
	else sr[0] &= ~SR_FLAG_V;
	//if (_sr & SR_FLAG_D) {
	if (0) {
		nb = ((a & 0x0F) + (operand & 0x0F) + (sr[0] & SR_FLAG_C));
		bcr = nb / 10;
		nb = nb % 10;
		a = (a / 16) + (operand / 16) + bcr;
		bcr = a / 10;
		a = a % 10;
		a <<= 4;
		a |= nb;
		ret = a;
	}
	else {
		ret = a + operand + (sr[0] & SR_FLAG_C);
		if (ret > 255) bcr = 1;
	}
	if (bcr) sr[0] |= SR_FLAG_C;
	else sr[0] &= ~SR_FLAG_C;
	return ret;
}

__forceinline uchar core_sub(int8 a, int8 operand, uchar* sr) {
	uchar nb = 0;
	register uchar op1;
	uchar temp;
	uint16 res;
	int16 ires;
	register uchar carry = 0;
	uchar bcr = 0;
	int16 ret = 0;
	uchar a_0;
	uchar a_1;
	uchar o_0;
	uchar o_1 = (operand & 0x0F) % 10;
	if (sub_is_overflow(a, operand)) sr[0] |= SR_FLAG_V;
	else sr[0] &= ~SR_FLAG_V;
	//if (_sr & SR_FLAG_D) {
	if (0) {
		a_0 = a / 16;
		a_1 = (a & 0x0F) % 10;
		o_0 = operand / 16;
		o_1 = (operand & 0x0F) % 10;
		if (a_1 < (o_1 + (sr[0] & SR_FLAG_C))) {
			nb = a_1 + 10 - (o_1 + (sr[0] & SR_FLAG_C));
			bcr = 1;
		}
		else {
			nb = a_1 - (o_1 + (sr[0] & SR_FLAG_C));
		}
		if (a_0 < (o_0 + bcr)) {
			a = a_0 + 10 - (o_0 + bcr);
			bcr = 1;
		}
		else {
			a = a_0 - (o_0 + bcr);
		}
		a = a % 10;
		a <<= 4;
		a |= nb;
		ret = a;
	}
	else {
		carry = !(sr[0] & SR_FLAG_C);
		op1 = _acc;
		res = (int16)((int8)op1 - carry) - (int16)operand;
		ires = ((uchar)op1 - (uchar)carry) - (uchar)operand;
		temp = ires;
		if (ires & 0x100) { sr[0] &= ~SR_FLAG_C; }
		else { sr[0] |= SR_FLAG_C; }
	}

	ret = temp;
	return ret;
}

uchar core_get_mem(uint16 address) {
	//need to implement other peripheral also

	switch (address) {
		//IO (PPU, DMA)
	case PPU_CR1:
		return ppu_get_cr1();
		break;
	case PPU_CR2:
		return ppu_get_cr2();
		break;
	case PPU_SR:
		return ppu_get_sr();
		break;
	case PPU_SPR_ADDR:
		return ppu_get_spr_addr();
		break;
	case PPU_SPR_DATA:
		return ppu_get_spr_data();
		break;
	case PPU_SCR_OFFSET:
		return ppu_get_scroll();
		break;
	case PPU_MEM_ADDR:
		return ppu_get_mem_addr();
		break;
	case PPU_MEM_DATA:
		return ppu_get_mem_data();
		break;
	case 0x4014:			//DMA
		break;
	default:
		return _sram[address];
		break;
	}
	return _sram[address];

}

uchar core_set_mem(uint16 address, uchar val) {
	//need to implement other peripheral also
	switch (address) {
		//IO (PPU, DMA)
	case PPU_CR1:
		ppu_set_cr1(val);
		break;
	case PPU_CR2:
		ppu_set_cr2(val);
		break;
	case PPU_SR:
		ppu_set_sr(val);
		break;
	case PPU_SPR_ADDR:
		ppu_set_spr_addr(val);
		break;
	case PPU_SPR_DATA:
		ppu_set_spr_data(val);
		break;
	case PPU_SCR_OFFSET:
		ppu_set_scroll(val);
		break;
	case PPU_MEM_ADDR:
		ppu_set_mem_addr(val);
		break;
	case PPU_MEM_DATA:
		ppu_set_mem_data(val);
		break;
	case 0x4014:			//DMA
		ppu_dma_write(_sram + (val * 0x100), 0x100);
		break;
	default:
		if (address & 0x8000) {
			if (_mmc.write != NULL) _mmc.write(_mmc.payload, address, val);
		} else {
			_sram[address] = val;
		}
		break;
	}
	return val;
}

uint16 core_get_word(uint16 address) {
	register uint16 hh = 0;
	register uchar ll = core_get_mem(address);
	if ((address & 0xFF) == 0xFF) {
		hh = core_get_mem(address & 0xFF00);
	}
	else {
		hh = core_get_mem(address + 1);
	}
	return (hh * 256) + ll;
}

void core_debug(char* opcode, uint8 operand, uint16 address) {
	printf("%04X : %s %04X\r\n", _pc, opcode, address);
}

#define USE_CARRY		(_sr & 0x01)
#define USE_CARRY		0
#define CPU_DEBUG(x)	//core_debug(x, operand, address);


void core_decode(uchar* opcodes) {
	uchar opcode = opcodes[0];
	register uchar operand = 0;
	register uint16 address = 0;
	register uint32 ptr;
	register uint16 lpc = _pc;
	register uchar lsp = _sp;
	register uchar lacc = _acc;
	register uchar lx = _x;
	register uchar ly = _y;
	register uchar psr = _sr;
	switch (opcode) {
	case 0x00:		//BRK
#if USE_ASM
		__asm {
			orr psr, psr, SR_FLAG_B
			sub operand, lsp, 1
			//sub lsp, lsp, 1
			add ptr, _stack, operand
			strh address, [ptr]
			//sub lsp, lsp, 1
			sub operand, operand, 1
			ldrb psr, [_stack, operand]
			sub operand, operand, 1
			mov lsp, operand
		}
#else
		psr |= SR_FLAG_B;			//set break flag
		_stack[lsp--] = (lpc + 2) >> 8;			//PCH
		_stack[lsp--] = (lpc + 2);				//PCL
		_stack[lsp--] = psr;					//SR
#endif
		CPU_DEBUG("BRK");
		//should jump to break interrupt vector (to do)
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x40:			//RTI		return from interrupt pull sr pull pc
#if USE_ASM
		__asm {
			//load sr from stack, clear break flag
			add lsp, lsp, 1
			ldrb psr, [_stack, lsp]
			and psr, psr, ~SR_FLAG_B
			//load address from stack	
			add lsp, lsp, 1
			add ptr, _stack, lsp
			ldrh address, [ptr]
			add lsp, lsp, 1
			//set pc to loaded address
			mov lpc, address

		}
#else
		psr = _stack[++lsp];
		psr &= ~SR_FLAG_B;			//clear break flag
		opcode = _stack[++lsp];
		address = _stack[++lsp];
		lpc = (((uint16)address << 8) | opcode);
#endif
		CPU_DEBUG("RTI");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x20:			//JSR
#if USE_ASM
		__asm {
			//calculate address = pc+2
			mov address, lpc
			add address, address, 2
			//store calculated address to stack
			sub lsp, lsp, 1
			add ptr, _stack, lsp
			strh address, [ptr]
			sub lsp, lsp, 1
			//load new address = [opcodes + 1]
			add ptr, opcodes, 1
			ldrh lpc, [ptr]
		}
#else
		_stack[lsp--] = (lpc + 2) >> 8;			//PCH
		_stack[lsp--] = (lpc + 2);				//PCL
		lpc = ((uint16)opcodes[2] * 256) + opcodes[1];		//absolute addressing mode
#endif
		CPU_DEBUG("JSR");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x60:			//RTS			return from subroutine
#if USE_ASM
		__asm {
			//load address from stack	
			add lsp, lsp, 1
			add ptr, _stack, lsp
			ldrh address, [ptr]
			add lsp, lsp, 1
			//set pc to loaded address
			add lpc, address, 1
		}
#else
		opcode = _stack[++lsp];
		address = _stack[++lsp];
		lpc = (((uint16)address << 8) | opcode) + 1;
#endif
		CPU_DEBUG("RTS");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x01:			//ORA Or with accumulator//(indirect, X)  (+2)
		address = (opcodes[1] + (uint16)lx) & 0xFF;
		lacc = core_orl(lacc, core_get_mem(core_get_word(address)));
		lpc += 2;
		CPU_DEBUG("ORA");
		break;
	case 0x05://zeropage  (+2)
		lacc = core_orl(lacc, core_get_mem(opcodes[1]));
		lpc += 2;
		CPU_DEBUG("ORA");
		break;
	case 0x09://immdt  (+2)
		lacc = core_orl(lacc, opcodes[1]);
		lpc += 2;
		break;
	case 0x0d://absolute (+3)
		lacc = core_orl(lacc, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]));
		lpc += 3;
		CPU_DEBUG("ORA");
		break;
	case 0x11://(indirect), Y  (+2)
		lacc = core_orl(lacc, core_get_mem((core_get_word(opcodes[1])) + (uint16)ly));
		lpc += 2;
		CPU_DEBUG("ORA");
		break;
	case 0x15://zeropage, X  (+2)
		address = (opcodes[1] + lx) & 0xFF;
		lacc = core_orl(lacc, core_get_mem(address));
		lpc += 2;
		CPU_DEBUG("ORA");
		break;
	case 0x19://absolute, Y (+3)
		lacc = core_orl(lacc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + ly));
		lpc += 3;
		CPU_DEBUG("ORA");
		break;
	case 0x1d://absolute, X (+3)
		lacc = core_orl(lacc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + lx));
		lpc += 3;
		CPU_DEBUG("ORA");
		break;
	case 0x0a:				//ASL arithmetic shift left
		//lacc = core_asl(lacc, 1);
		ptr = (uint16)lacc << 1;
		if (ptr & 0x100) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		lacc = ptr;
		lpc += 1;
		CPU_DEBUG("ASL");
		break;
	case 0x06:					//ASL arithmetic shift left//zeropage  (+2)
		//operand = core_asl(core_get_mem(opcodes[1]), 1);
		address = opcodes[1];
		operand = core_get_mem(address);
		ptr = (uint16)operand << 1;
		if (ptr & 0x100) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		core_set_mem(address, ptr);
		lpc += 2;
		CPU_DEBUG("ASL");
		break;
	case 0x0e://absolute (+3)
		//operand = core_asl(core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), 1);
		address = ((uint16)opcodes[2] * 256) + opcodes[1];
		operand = core_get_mem(address);
		ptr = (uint16)operand << 1;
		if (ptr & 0x100) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		core_set_mem(address, ptr);
		//core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
		lpc += 3;
		CPU_DEBUG("ASL");
		break;
	case 0x16:	//zeropage, X  (+2)
		address = (opcodes[1] + lx) & 0xFF;
		//operand = core_asl(core_get_mem(address), 1);
		operand = core_get_mem(address);
		ptr = (uint16)operand << 1;
		if (ptr & 0x100) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		core_set_mem(address, ptr);
		//core_set_mem(address, operand);
		lpc += 2;
		CPU_DEBUG("ASL");
		break;
	case 0x1e://absolute, X (+3)
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + lx;
		//operand = core_asl(core_get_mem(address), 1);
		operand = core_get_mem(address);
		ptr = (uint16)operand << 1;
		if (ptr & 0x100) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		core_set_mem(address, ptr);
		//core_set_mem(address, operand);
		lpc += 3;
		CPU_DEBUG("ASL");
		break;
	case 0x08:			//PHP			push status register
		_stack[lsp--] = psr;
		lpc += 1;
		CPU_DEBUG("PHP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x28:			//PLP			pull status register		10
		psr = _stack[++lsp];
		psr &= ~SR_FLAG_B;
		lpc += 1;
		CPU_DEBUG("PLP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x48:			//PHA			push accumulator
		_stack[lsp--] = lacc;
		lpc += 1;
		CPU_DEBUG("PHA");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x68:			//PLA			pull accumulator
		lacc = _stack[++lsp];
		lpc += 1;
		CPU_DEBUG("PLA");
		break;
	case 0x18:			//CLC
		psr &= ~SR_FLAG_C;
		lpc += 1;
		CPU_DEBUG("CLC");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x58:			//CLI
		psr &= ~SR_FLAG_I;
		lpc += 1;
		CPU_DEBUG("CLI");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0xB8:			//CLV
		psr &= ~SR_FLAG_V;
		lpc += 1;
		CPU_DEBUG("CLV");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0xD8:			//CLD
		psr &= ~SR_FLAG_D;
		lpc += 1;
		CPU_DEBUG("CLD");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x24:			//BIT//zeropage
		operand = core_get_mem(opcodes[1]);
		if (operand & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		if (operand & 0x40) psr |= SR_FLAG_V;
		else psr &= ~SR_FLAG_V;
		if (core_and(lacc, operand) == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		lpc += 2;
		CPU_DEBUG("BIT");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0x2c://absolute
		operand = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
		if (operand & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		if (operand & 0x40) psr |= SR_FLAG_V;
		else psr &= ~SR_FLAG_V;
		if (core_and(lacc, operand) == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		lpc += 3;
		CPU_DEBUG("BIT");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)

	case 0x21:			//AND//(indirect, X)  (+2)
		address = (opcodes[1] + (uint16)lx) & 0xFF;
		lacc = core_and(lacc, core_get_mem(core_get_word(address)));
		lpc += 2;
		CPU_DEBUG("AND");
		break;
	case 0x25:	//zeropage  (+2)
		lacc = core_and(lacc, core_get_mem(opcodes[1]));
		lpc += 2;
		CPU_DEBUG("AND");
		break;
	case 0x29:	//immdt  (+2)
		lacc = core_and(lacc, opcodes[1]);
		lpc += 2;
		CPU_DEBUG("AND");
		break;
	case 0x2d:	//absolute (+3)
		lacc = core_and(lacc, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]));
		lpc += 3;
		CPU_DEBUG("AND");
		break;
	case 0x31:	//(indirect), Y  (+2)
		lacc = core_and(lacc, core_get_mem((core_get_word(opcodes[1])) + (uint16)ly));
		lpc += 2;
		CPU_DEBUG("AND");
		break;
	case 0x35:	//zeropage, X  (+2)
		lacc = core_and(lacc, core_get_mem((opcodes[1] + lx) & 0xFF));
		lpc += 2;
		CPU_DEBUG("AND");
		break;
	case 0x39:	//absolute, Y (+3)
		lacc = core_and(lacc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + ly));
		lpc += 3;
		CPU_DEBUG("AND");
		break;
	case 0x3d:	//absolute, X (+3)
		lacc = core_and(lacc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + lx));
		lpc += 3;
		CPU_DEBUG("AND");
		break;

	case 0x2a:			//ROL accumulator
		//lacc = core_rol(lacc, 1);
		ptr = (uint16)lacc << 1;
		ptr |= (psr & SR_FLAG_C);
		if (ptr & 0x100) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		lacc = ptr;
		lpc += 1;
		CPU_DEBUG("ROL");
		break;
	case 0x26:			//ROL	//zeropage  (+2)
		address = opcodes[1];
		//operand = core_rol(core_get_mem(opcodes[1]), 1);
		operand = core_get_mem(opcodes[1]);
		ptr = (uint16)operand << 1;
		ptr |= (psr & SR_FLAG_C);
		if (ptr & 0x100) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		core_set_mem(address, ptr);
		//core_set_mem(opcodes[1], operand);
		lpc += 2;
		CPU_DEBUG("ROL");
		break;
	case 0x2e://absolute (+3)
		address = ((uint16)opcodes[2] * 256) + opcodes[1];
		//operand = core_rol(core_get_mem(address), 1);
		operand = core_get_mem(address);
		ptr = (uint16)operand << 1;
		ptr |= (psr & SR_FLAG_C);
		if (ptr & 0x100) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		core_set_mem(address, ptr);
		//core_set_mem(address, operand);
		lpc += 3;
		CPU_DEBUG("ROL");
		break;
	case 0x36://zeropage, X  (+2)
		address = (opcodes[1] + lx) & 0xFF;
		//operand = core_rol(core_get_mem(address), 1);
		operand = core_get_mem(address);
		ptr = (uint16)operand << 1;
		ptr |= (psr & SR_FLAG_C);
		if (ptr & 0x100) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		core_set_mem(address, ptr);
		//core_set_mem(address, operand);
		lpc += 2;
		CPU_DEBUG("ROL");
		break;
	case 0x3e:	//absolute, X (+3)
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + lx;
		//operand = core_rol(core_get_mem(address), 1);
		operand = core_get_mem(address);
		ptr = (uint16)operand << 1;
		ptr |= (psr & SR_FLAG_C);
		if (ptr & 0x100) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		core_set_mem(address, ptr);
		//core_set_mem(address, operand);
		lpc += 3;
		CPU_DEBUG("ROL");
		break;

	case 0x4c:			//JMP	//absolute
		lpc = ((uint16)opcodes[2] * 256) + opcodes[1];
		CPU_DEBUG("JMP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0x6c://indirect
		if (opcodes[1] != 0xFF) {
			lpc = core_get_word(((uint16)opcodes[2] * 256) + opcodes[1]);
		}
		else {
			operand = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
			address = core_get_mem(((uint16)opcodes[2] * 256));
			address = (address << 8) | operand;
			lpc = address;
		}
		CPU_DEBUG("JMP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)

	case 0x41:			//EOR//(indirect, X)  (+2)
		address = (opcodes[1] + (uint16)lx) & 0xFF;
		lacc = core_xor(lacc, core_get_mem(core_get_word(address)));
		lpc += 2;
		CPU_DEBUG("EOR");
		break;
	case 0x45:	//zeropage  (+2)
		lacc = core_xor(lacc, core_get_mem(opcodes[1]));
		lpc += 2;
		break;
	case 0x49:	//immdt  (+2)
		lacc = core_xor(lacc, opcodes[1]);
		lpc += 2;
		CPU_DEBUG("EOR");
		break;
	case 0x4d:	//absolute (+3)
		lacc = core_xor(lacc, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]));
		lpc += 3;
		CPU_DEBUG("EOR");
		break;
	case 0x51:	//(indirect), Y  (+2)
		lacc = core_xor(lacc, core_get_mem((core_get_word(opcodes[1])) + (uint16)ly));
		lpc += 2;
		CPU_DEBUG("EOR");
		break;
	case 0x55:	//zeropage, X  (+2)
		lacc = core_xor(lacc, core_get_mem((opcodes[1] + lx) & 0xFF));
		lpc += 2;
		CPU_DEBUG("EOR");
		break;
	case 0x59:	//absolute, Y (+3)
		lacc = core_xor(lacc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + ly));
		lpc += 3;
		CPU_DEBUG("EOR");
		break;
	case 0x5d:	//absolute, X (+3)
		lacc = core_xor(lacc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + lx));
		lpc += 3;
		CPU_DEBUG("EOR");
		break;

	case 0x4a:			//LSR accumulator
		//lacc = core_lsr(lacc, 1);
		if (lacc & 0x01) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		lacc = (uint16)lacc >> 1;
		lpc += 1;
		CPU_DEBUG("LSR");
		break;
	case 0x46:			//LSR//zeropage  (+2)
		//operand = core_lsr(core_get_mem(opcodes[1]), 1);
		address = opcodes[1];
		operand = core_get_mem(address);
		if (operand & 0x01) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		operand = (uint16)operand >> 1;
		core_set_mem(address, operand);
		//core_set_mem(opcodes[1], operand);
		lpc += 2;
		CPU_DEBUG("LSR");
		break;
	case 0x4e://absolute (+3)
		//operand = core_lsr(core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), 1);
		address = ((uint16)opcodes[2] * 256) + opcodes[1];
		operand = core_get_mem(address);
		if (operand & 0x01) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		operand = (uint16)operand >> 1;
		core_set_mem(address, operand);
		//core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
		lpc += 3;
		CPU_DEBUG("LSR");
		break;
	case 0x56://zeropage, X  (+2)
		address = (opcodes[1] + lx) & 0xFF;
		//operand = core_lsr(core_get_mem(address), 1);
		operand = core_get_mem(address);
		if (operand & 0x01) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		operand = (uint16)operand >> 1;
		core_set_mem(address, operand);
		//core_set_mem(address, operand);
		lpc += 2;
		CPU_DEBUG("LSR");
		break;
	case 0x5e://absolute, X (+3)
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + lx;
		//operand = core_lsr(core_get_mem(address), 1);
		operand = core_get_mem(address);
		if (operand & 0x01) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		operand = (uint16)operand >> 1;
		core_set_mem(address, operand);
		//core_set_mem(address, operand);
		lpc += 3;
		CPU_DEBUG("LSR");
		break;

	case 0x61:			//ADC		22//(indirect, X)  (+2)
		address = (opcodes[1] + (uint16)lx) & 0xFF;
		lacc = core_add(lacc, core_get_mem(core_get_word(address)), &psr);
		lpc += 2;
		CPU_DEBUG("ADC");
		break;
	case 0x65:	//zeropage  (+2)
		lacc = core_add(lacc, core_get_mem(opcodes[1]), &psr);
		lpc += 2;
		break;
	case 0x69:	//immdt  (+2)
		lacc = core_add(lacc, opcodes[1], &psr);
		lpc += 2;
		CPU_DEBUG("ADC");
		break;
	case 0x6d:	//absolute (+3)
		lacc = core_add(lacc, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), &psr);
		lpc += 3;
		CPU_DEBUG("ADC");
		break;
	case 0x71:	//(indirect), Y  (+2)
		lacc = core_add(lacc, core_get_mem((core_get_word(opcodes[1])) + (uint16)ly), &psr);
		lpc += 2;
		CPU_DEBUG("ADC");
		break;
	case 0x75:	//zeropage, X  (+2)
		lacc = core_add(lacc, core_get_mem((opcodes[1] + lx) & 0xFF), &psr);
		lpc += 2;
		CPU_DEBUG("ADC");
		break;
	case 0x79:	//absolute, Y (+3)
		lacc = core_add(lacc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + ly), &psr);
		lpc += 3;
		CPU_DEBUG("ADC");
		break;
	case 0x7d:	//absolute, X (+3)
		lacc = core_add(lacc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + lx), &psr);
		lpc += 3;
		CPU_DEBUG("ADC");
		break;

	case 0x6a:			//ROR accumulator
		//lacc = core_ror(lacc, 1);
		//csr = (psr & SR_FLAG_C);
		ptr = lacc;
		if ((psr & SR_FLAG_C)) ptr |= 0x100;
		if (ptr & 0x01) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		lacc = (uint16)ptr >> 1;
		lpc += 1;
		CPU_DEBUG("ROR");
		break;
	case 0x66:			//ROR//zeropage  (+2)
		address = opcodes[1];
		ptr = core_get_mem(address);
		//operand = core_ror(core_get_mem(opcodes[1]), 1);
		if ((psr & SR_FLAG_C)) ptr |= 0x100;
		if (ptr & 0x01) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		ptr = (uint16)ptr >> 1;
		core_set_mem(address, ptr);
		lpc += 2;
		CPU_DEBUG("ROR");
		break;
	case 0x6e://absolute (+3)
		address = ((uint16)opcodes[2] * 256) + opcodes[1];
		ptr = core_get_mem(address);
		//operand = core_ror(core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), 1);
		if ((psr & SR_FLAG_C)) ptr |= 0x100;
		if (ptr & 0x01) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		ptr = (uint16)ptr >> 1;
		core_set_mem(address, ptr);
		lpc += 3;
		CPU_DEBUG("ROR");
		break;
	case 0x76://zeropage, X  (+2)
		address = (opcodes[1] + lx) & 0xFF;
		ptr = core_get_mem(address);
		//operand = core_ror(core_get_mem(address), 1);
		if ((psr & SR_FLAG_C)) ptr |= 0x100;
		if (ptr & 0x01) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		ptr = (uint16)ptr >> 1;
		core_set_mem(address, ptr);
		//core_set_mem(address, operand);
		lpc += 2;
		CPU_DEBUG("ROR");
		break;
	case 0x7e://absolute, X (+3)
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + lx;
		ptr = core_get_mem(address);
		//operand = core_ror(core_get_mem(address), 1);
		if ((psr & SR_FLAG_C)) ptr |= 0x100;
		if (ptr & 0x01) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		ptr = (uint16)ptr >> 1;
		core_set_mem(address, ptr);
		//core_set_mem(address, operand);
		lpc += 3;
		CPU_DEBUG("ROR");
		break;

	case 0x81:			//STA//(indirect, X)  (+2)
		address = (opcodes[1] + (uint16)lx) & 0xFF;
		core_set_mem(core_get_word(address), lacc);
		lpc += 2;
		CPU_DEBUG("STA");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0x85://zeropage  (+2)
		core_set_mem(opcodes[1], lacc);
		lpc += 2;
		CPU_DEBUG("STA");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0x8d://absolute (+3)
		core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], lacc);
		lpc += 3;
		CPU_DEBUG("STA");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0x91://(indirect), Y  (+2)
		core_set_mem((core_get_word(opcodes[1])) + (uint16)ly, lacc);
		lpc += 2;
		CPU_DEBUG("STA");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0x95://zeropage, X  (+2)
		core_set_mem((opcodes[1] + lx) & 0xFF, lacc);
		lpc += 2;
		CPU_DEBUG("STA");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0x99:	//absolute, Y (+3)
		core_set_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + ly, lacc);
		lpc += 3;
		CPU_DEBUG("STA");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0x9d:	//absolute, X (+3)
		core_set_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + lx, lacc);
		lpc += 3;
		CPU_DEBUG("STA");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)

	case 0x84:			//STY	//zeropage  (+2)
		core_set_mem(opcodes[1], ly);
		lpc += 2;
		CPU_DEBUG("STY");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0x8c://absolute (+3)
		core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], ly);
		lpc += 3;
		CPU_DEBUG("STY");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0x94:	//zeropage, X  (+2)
		core_set_mem((opcodes[1] + lx) & 0xFF, ly);
		lpc += 2;
		CPU_DEBUG("STY");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)

	case 0x86:			//STX//zeropage  (+2)
		core_set_mem(opcodes[1], lx);
		lpc += 2;
		CPU_DEBUG("STX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0x8e://absolute (+3)
		core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], lx);
		lpc += 3;
		CPU_DEBUG("STX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0x96://zeropage, Y  (+2)
		core_set_mem((opcodes[1] + ly) & 0xFF, lx);
		lpc += 2;
		CPU_DEBUG("STX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)

	case 0xA0:			//LDY
		ly = opcodes[1];
		lpc += 2;
		if (ly == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (ly & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("LDY");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0xA4:			//LDY	//zeropage  (+2)
		ly = core_get_mem(opcodes[1]);
		lpc += 2;
		if (ly == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (ly & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("LDY");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xAc://absolute (+3)
		ly = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
		lpc += 3;
		if (ly == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (ly & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("LDY");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xB4://zeropage, X  (+2)
		ly = core_get_mem((opcodes[1] + lx) & 0xFF);
		lpc += 2;
		if (ly == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (ly & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("LDY");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xBc://absolute, X (+3)
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + lx;
		ly = core_get_mem(address);
		lpc += 3;
		if (ly == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (ly & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("LDY");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)

	case 0xA1:			//LDA//(indirect, X)  (+2)
		address = (opcodes[1] + (uint16)lx) & 0xFF;
		lacc = core_lda(lacc, core_get_mem(core_get_word(address)));
		lpc += 2;
		CPU_DEBUG("LDA");
		break;
	case 0xA5:	//zeropage  (+2)
		address = opcodes[1];
		lacc = core_lda(lacc, core_get_mem(address));
		lpc += 2;
		CPU_DEBUG("LDA");
		break;
	case 0xA9:	//immdt  (+2)
		lacc = core_lda(lacc, opcodes[1]);
		lpc += 2;
		CPU_DEBUG("LDA");
		break;
	case 0xAd://absolute (+3)
		address = ((uint16)opcodes[2] * 256) + opcodes[1];
		lacc = core_lda(lacc, core_get_mem(address));
		lpc += 3;
		CPU_DEBUG("LDA");
		break;
	case 0xB1:	//(indirect), Y  (+2)
		address = (core_get_word(opcodes[1])) + (uint16)ly;
		lacc = core_lda(lacc, core_get_mem(address));
		lpc += 2;
		CPU_DEBUG("LDA");
		break;
	case 0xB5:	//zeropage, X  (+2)
		address = (opcodes[1] + lx) & 0xFF;
		lacc = core_lda(lacc, core_get_mem(address));
		lpc += 2;
		CPU_DEBUG("LDA");
		break;
	case 0xB9:		//absolute, Y (+3)
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + ly;
		lacc = core_lda(lacc, core_get_mem(address));
		lpc += 3;
		CPU_DEBUG("LDA");
		break;
	case 0xBd:	//absolute, X (+3)
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + lx;
		lacc = core_lda(lacc, core_get_mem(address));
		lpc += 3;
		CPU_DEBUG("LDA");
		break;

	case 0xA2:			//LDX
		lx = opcodes[1];
		lpc += 2;
		if (lx == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (lx & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("LDX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xA6:	//zeropage  (+2)
		lx = core_get_mem(opcodes[1]);
		lpc += 2;
		if (lx == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (lx & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("LDX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xAe:	//absolute (+3)
		lx = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
		lpc += 3;
		if (lx == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (lx & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("LDX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xB6:	//zeropage, Y  (+2)
		lx = core_get_mem((opcodes[1] + ly) & 0xFF);
		lpc += 2;
		if (lx == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (lx & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("LDX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xBe:	//absolute, Y (+3)
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + ly;
		lx = core_get_mem(address);
		lpc += 3;
		if (lx == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (lx & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("LDX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)

	case 0xA8:			//TAY		accumulator to y
		ly = lacc;
		lpc += 1;
		if (ly == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (ly & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("TAY");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0xAA:			//TAX		accumulator to x
		lx = lacc;
		lpc += 1;
		if (lx == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (lx & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("TAX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0xBA:			//TSX		sp to x
		lx = lsp;
		lpc += 1;
		if (lx == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (lx & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("TSX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x8A:			//TXA		x to accumulator
		lacc = lx;
		lpc += 1;
		CPU_DEBUG("TXA");
		break;
	case 0x98:			//TYA		y to accumulator
		lacc = ly;
		lpc += 1;
		CPU_DEBUG("TYA");
		break;
	case 0x9A:			//TXS		x to stack pointer
		lsp = lx;
		lpc += 1;
		CPU_DEBUG("TXS");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0xC0:			//CPY	//immediate  (+2)
		core_cmp(ly, opcodes[1], psr);
		lpc += 2;
		CPU_DEBUG("CPY");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xC4://zeropage  (+2)
		core_cmp(ly, core_get_mem(opcodes[1]), psr);
		lpc += 2;
		CPU_DEBUG("CPY");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xCc://absolute (+3)
		core_cmp(ly, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), psr);
		lpc += 3;
		CPU_DEBUG("CPY");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)

	case 0xC1:			//CMP//(indirect, X)  (+2)
		address = (opcodes[1] + (uint16)lx) & 0xFF;
		core_cmp(lacc, core_get_mem(core_get_word(address)), psr);
		lpc += 2;
		CPU_DEBUG("CMP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xC5://zeropage  (+2)
		core_cmp(lacc, core_get_mem(opcodes[1]), psr);
		lpc += 2;
		CPU_DEBUG("CMP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xC9:	//immdt  (+2)
		core_cmp(lacc, opcodes[1], psr);
		lpc += 2;
		CPU_DEBUG("CMP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xCd:	//absolute (+3)
		core_cmp(lacc, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), psr);
		lpc += 3;
		CPU_DEBUG("CMP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xd1:	//(indirect), Y  (+2)
		core_cmp(lacc, core_get_mem((core_get_word(opcodes[1])) + (uint16)ly), psr);
		lpc += 2;
		CPU_DEBUG("CMP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xd5:	//zeropage, X  (+2)
		core_cmp(lacc, core_get_mem((opcodes[1] + lx) & 0xFF), psr);
		lpc += 2;
		CPU_DEBUG("CMP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xd9:	//absolute, Y (+3)
		core_cmp(lacc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + ly), psr);
		lpc += 3;
		CPU_DEBUG("CMP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xdd:	//absolute, X (+3)
		core_cmp(lacc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + lx), psr);
		lpc += 3;
		CPU_DEBUG("CMP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)

	case 0xC6:			//DEC//zeropage  (+2)
		operand = core_get_mem(opcodes[1]);
		operand--;
		core_set_mem(opcodes[1], operand);
		lpc += 2;
		if (operand == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (operand & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("DEC");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xCE:	//absolute (+3)
		operand = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
		operand--;
		core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
		lpc += 3;
		if (operand == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (operand & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("DEC");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xD6:		//zeropage, X  (+2)
		address = (opcodes[1] + lx) & 0xFF;
		operand = core_get_mem(address);
		operand--;
		core_set_mem(address, operand);
		lpc += 2;
		if (operand == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (operand & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("DEC");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xDE:	//absolute, X (+3)
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + lx;
		operand = core_get_mem(address);
		operand--;
		core_set_mem(address, operand);
		lpc += 3;
		if (operand == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (operand & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("DEC");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)

	case 0xC8:			//INY		increment y
		ly++;
		lpc += 1;
		if (ly == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (ly & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("INY");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0xCA:			//DEX		decrement x
		lx--;
		lpc += 1;
		if (lx == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (lx & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("DEX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x88:			//DEY			30
		ly--;
		lpc += 1;
		if (ly == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (ly & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("DEY");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0xe8:			//INX
		lx++;
		lpc += 1;
		if (lx == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (lx & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("INX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0xE0:			//CPX			50//immediate  (+2)
		core_cmp(lx, opcodes[1], psr);
		lpc += 2;
		CPU_DEBUG("CPX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xE4:		//zeropage  (+2)
		core_cmp(lx, core_get_mem(opcodes[1]), psr);
		lpc += 2;
		CPU_DEBUG("CPX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xEC:	//absolute (+3)
		core_cmp(lx, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), psr);
		lpc += 3;
		CPU_DEBUG("CPX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)

	case 0xE1:			//SBC	//(indirect, X)  (+2)
		address = (opcodes[1] + (uint16)lx) & 0xFF;
		lacc = core_sub(lacc, core_get_mem(core_get_word(address)), &psr);
		lpc += 2;
		CPU_DEBUG("SBC");
		break;
	case 0xE5://zeropage  (+2)
		lacc = core_sub(lacc, core_get_mem(opcodes[1]), &psr);
		lpc += 2;
		CPU_DEBUG("SBC");
		break;
	case 0xE9:	//immdt  (+2)
		lacc = core_sub(lacc, opcodes[1], &psr);
		lpc += 2;
		CPU_DEBUG("SBC");
		break;
	case 0xED:	//absolute (+3)
		lacc = core_sub(lacc, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), &psr);
		lpc += 3;
		CPU_DEBUG("SBC");
		break;
	case 0xF1:	//(indirect), Y  (+2)
		lacc = core_sub(lacc, core_get_mem((core_get_word(opcodes[1])) + (uint16)ly), &psr);
		lpc += 2;
		CPU_DEBUG("SBC");
		break;
	case 0xF5:		//zeropage, X  (+2)
		lacc = core_sub(lacc, core_get_mem((opcodes[1] + lx) & 0xFF), &psr);
		lpc += 2;
		CPU_DEBUG("SBC");
		break;
	case 0xF9:		//absolute, Y (+3)
		lacc = core_sub(lacc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + ly), &psr);
		lpc += 3;
		CPU_DEBUG("SBC");
		break;
	case 0xFD:	//absolute, X (+3)
		lacc = core_sub(lacc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + lx), &psr);
		lpc += 3;
		CPU_DEBUG("SBC");
		break;

	case 0xe6:			//INC//zeropage  (+2)
		operand = core_get_mem(opcodes[1]);
		operand++;
		core_set_mem(opcodes[1], operand);
		lpc += 2;
		if (operand == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (operand & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("INC");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xeE:	//absolute (+3)
		operand = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
		operand++;
		core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
		lpc += 3;
		if (operand == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (operand & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("INC");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xf6:	//zeropage, X  (+2)
		address = (opcodes[1] + lx) & 0xFF;
		operand = core_get_mem(address);
		operand++;
		core_set_mem(address, operand);
		lpc += 2;
		if (operand == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (operand & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("INC");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
	case 0xfE:	//absolute, X (+3)
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + lx;
		operand = core_get_mem(address);
		operand++;
		core_set_mem(address, operand);
		lpc += 3;
		if (operand == 0) psr |= SR_FLAG_Z;
		else psr &= ~SR_FLAG_Z;
		if (operand & 0x80) psr |= SR_FLAG_N;
		else psr &= ~SR_FLAG_N;
		CPU_DEBUG("INC");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)

	case 0xea:			//NOP
		lpc += 1;
		CPU_DEBUG("NOP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0xF0:			//BEQ  branch on equal
		if ((psr & SR_FLAG_Z)) lpc += (int8)opcodes[1];
		lpc += 2;
		CPU_DEBUG("BEQ");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0xD0:			//BNE  branch on not equal
		if ((psr & SR_FLAG_Z) == 0) lpc += (int8)opcodes[1];
		lpc += 2;
		CPU_DEBUG("BNE");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0xB0:			//BCS	branch on carry set	40
		if ((psr & SR_FLAG_C)) lpc += (int8)opcodes[1];
		lpc += 2;
		CPU_DEBUG("BCS");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x90:			//BCC  branch on carry clear
		if ((psr & SR_FLAG_C) == 0) lpc += (int8)opcodes[1];
		lpc += 2;
		CPU_DEBUG("BCC");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x70:			//BVS   branch on overflow set
		if ((psr & SR_FLAG_V)) lpc += (int8)opcodes[1];
		lpc += 2;
		CPU_DEBUG("BVS");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x50:			//BVC  branch on overflow clear
		if ((psr & SR_FLAG_V) == 0) lpc += (int8)opcodes[1];
		lpc += 2;
		CPU_DEBUG("BVC");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x30:			//BMI   branch on minus
		if ((psr & SR_FLAG_N)) lpc += (int8)opcodes[1];
		lpc += 2;
		CPU_DEBUG("BMI");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x10:			//BPL branch on plus
		if ((psr & SR_FLAG_N) == 0) lpc += (int8)opcodes[1];
		lpc += 2;
		CPU_DEBUG("BPL");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0xF8:			//SED
		psr |= SR_FLAG_D;
		lpc += 1;
		CPU_DEBUG("SED");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x78:			//SEI
		psr |= SR_FLAG_I;
		lpc += 1;
		CPU_DEBUG("SEI");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x38:			//SEC
		psr |= SR_FLAG_C;
		lpc += 1;
		CPU_DEBUG("SEC");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;

		//ILLEGAL INSTRUCTION CODES
	case 0x1A:			//illegal nop
	case 0x3A:
	case 0x5A:
	case 0x7A:
	case 0xDA:
	case 0xFA:
		lpc += 1;
		CPU_DEBUG("NOP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x04:			//illegal nop
	case 0x44:
	case 0x64:
	case 0x14:
	case 0x34:
	case 0x54:
	case 0x74:
	case 0xd4:
	case 0xf4:
	case 0x80:
	case 0x82:
	case 0x89:
		lpc += 2;
		CPU_DEBUG("NOP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0x0c:			//illegal nop
	case 0x1c:
	case 0x3c:
	case 0x5c:
	case 0x7c:
	case 0xdc:
	case 0xfc:
		lpc += 3;
		CPU_DEBUG("NOP");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0xAB:			//LAX #immdt
		lacc = core_lda(lacc, opcodes[1]);
		lx = lacc;
		lpc += 2;
		CPU_DEBUG("LAX");
		break;
	case 0xA7:			//LAX
	case 0xB7:
	case 0xAF:
	case 0xBF:
	case 0xA3:
	case 0xB3:
		switch (opcode & 0x1c) {
		case 0x00:		//(indirect, X)  (+2)
			address = (opcodes[1] + (uint16)lx) & 0xFF;
			lacc = core_lda(lacc, core_get_mem(core_get_word(address)));
			lpc += 2;
			break;
		case 0x04:		//zeropage  (+2)
			lacc = core_lda(lacc, core_get_mem(opcodes[1]));
			lpc += 2;
			break;
		case 0x0c:		//absolute (+3)
			lacc = core_lda(lacc, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]));
			lpc += 3;
			break;
		case 0x10:		//(indirect), Y  (+2)
			lacc = core_lda(lacc, core_get_mem((core_get_word(opcodes[1])) + (uint16)ly));
			lpc += 2;
			break;
		case 0x14:		//zeropage, Y  (+2)
			lacc = core_lda(lacc, core_get_mem((opcodes[1] + ly) & 0xff));
			lpc += 2;
			break;
		case 0x1c:		//absolute, Y (+3)
			address = (((uint16)opcodes[2] * 256) + opcodes[1]) + ly;
			lacc = core_lda(lacc, core_get_mem(address));
			lpc += 3;
			break;
		}
		lx = lacc;
		CPU_DEBUG("LAX");
		break;
	case 0x83:			//SAX
	case 0x87:
	case 0x8F:
	case 0x97:
		operand = lacc & lx;
		switch (opcode & 0x1c) {
		case 0x00:		//(indirect, X)  (+2)
			address = (opcodes[1] + (uint16)lx) & 0xFF;
			core_set_mem(core_get_word(address), operand);
			lpc += 2;
			break;
		case 0x04:		//zeropage  (+2)
			core_set_mem(opcodes[1], operand);
			lpc += 2;
			break;
		case 0x0c:		//absolute (+3)
			core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
			lpc += 3;
			break;
		case 0x14:		//zeropage, Y  (+2)
			core_set_mem((opcodes[1] + ly) & 0xFF, operand);
			lpc += 2;
			break;
		}
		CPU_DEBUG("SAX");
		goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		//break;
	case 0xDB:			//DCP
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + ly;
		operand = core_get_mem(address);
		core_set_mem(address, operand - 1);
		lpc += 3;
		core_cmp(lacc, operand - 1, psr);

		CPU_DEBUG("DCP");
		goto skip_flag_test;
		//break;
	case 0xC7:			//DCP
	case 0xD7:
	case 0xCF:
	case 0xDF:
	case 0xC3:
	case 0xD3:
		switch (opcode & 0x1C) {
		case 0x00:		//(indirect, X)  (+2)
			address = (opcodes[1] + (uint16)lx) & 0xFF;
			operand = core_get_mem(core_get_word(address));
			core_set_mem(core_get_word(address), operand - 1);
			lpc += 2;
			break;
		case 0x10:		//(indirect), Y  (+2)
			address = (core_get_word(opcodes[1])) + (uint16)ly;
			operand = core_get_mem(address);
			core_set_mem(address, operand - 1);
			lpc += 2;
			break;
		case 0x04:		//zeropage  (+2)
			operand = core_get_mem(opcodes[1]);
			core_set_mem(opcodes[1], operand - 1);
			lpc += 2;
			break;
		case 0x0c:		//absolute (+3)
			operand = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
			core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand - 1);
			lpc += 3;
			break;
		case 0x14:		//zeropage, X  (+2)
			address = (opcodes[1] + lx) & 0xFF;
			operand = core_get_mem(address);
			core_set_mem(address, operand - 1);
			lpc += 2;
			break;
		case 0x1c:		//absolute, X (+3)
			address = (((uint16)opcodes[2] * 256) + opcodes[1]) + lx;
			operand = core_get_mem(address);
			core_set_mem(address, operand - 1);
			lpc += 3;
			break;
		}
		core_cmp(lacc, operand - 1, psr);
		CPU_DEBUG("DCP");
		goto skip_flag_test;
		//break;
	case 0x3B:				//RLA  absolute, Y (+3)
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + ly;
		operand = core_get_mem(address);
		//operand = core_rol(core_get_mem(address), 1);
		ptr = (uint16)operand << 1;
		ptr |= (psr & SR_FLAG_C);
		if (ptr & 0x100) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		core_set_mem(address, ptr);
		lpc += 3;
		lacc = core_and(lacc, (uchar)ptr);
		CPU_DEBUG("RLA");
		break;
	case 0x27:				//RLA
	case 0x37:
	case 0x2F:
	case 0x3F:
	case 0x23:
	case 0x33:
		switch (opcode & 0x1c) {
		case 0x00:		//(indirect, X)  (+2)
			address = (opcodes[1] + (uint16)lx) & 0xFF;
			address = core_get_word(address);
			operand = core_get_mem(address);
			//operand = core_rol(core_get_mem(core_get_word(address)), 1);
			ptr = (uint16)operand << 1;
			ptr |= (psr & SR_FLAG_C);
			if (ptr & 0x100) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			core_set_mem(address, ptr);
			//core_set_mem(add, operand);
			lpc += 2;
			break;
		case 0x10:		//(indirect), Y  (+2)
			address = (core_get_word(opcodes[1])) + (uint16)ly;
			operand = core_get_mem(address);
			//operand = core_rol(core_get_mem(address), 1);
			ptr = (uint16)operand << 1;
			ptr |= (psr & SR_FLAG_C);
			if (ptr & 0x100) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			core_set_mem(address, ptr);
			//core_set_mem(address, operand);
			lpc += 2;
			break;
		case 0x04:		//zeropage  (+2)
			address = opcodes[1];
			operand = core_get_mem(address);
			//operand = core_rol(core_get_mem(opcodes[1]), 1);
			ptr = (uint16)operand << 1;
			ptr |= (psr & SR_FLAG_C);
			if (ptr & 0x100) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			core_set_mem(address, ptr);
			//core_set_mem(opcodes[1], operand);
			lpc += 2;
			break;
		case 0x0c:		//absolute (+3)
			address = ((uint16)opcodes[2] * 256) + opcodes[1];
			operand = core_get_mem(address);
			//operand = core_rol(core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), 1);
			ptr = (uint16)operand << 1;
			ptr |= (psr & SR_FLAG_C);
			if (ptr & 0x100) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			core_set_mem(address, ptr);
			//core_set_mem(address, operand);
			lpc += 3;
			break;
		case 0x14:		//zeropage, X  (+2)
			address = (opcodes[1] + lx) & 0xFF;
			operand = core_get_mem(address);
			//operand = core_rol(core_get_mem(address), 1);
			ptr = (uint16)operand << 1;
			ptr |= (psr & SR_FLAG_C);
			if (ptr & 0x100) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			core_set_mem(address, ptr);
			//core_set_mem(address, operand);
			lpc += 2;
			break;
		case 0x1c:		//absolute, X (+3)
			address = (((uint16)opcodes[2] * 256) + opcodes[1]) + lx;
			operand = core_get_mem(address);
			//operand = core_rol(core_get_mem(address), 1);
			ptr = (uint16)operand << 1;
			ptr |= (psr & SR_FLAG_C);
			if (ptr & 0x100) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			core_set_mem(address, ptr);
			//core_set_mem(address, operand);
			lpc += 3;
			break;
		}
		lacc = core_and(lacc, operand);
		CPU_DEBUG("RLA");
		break;
	case 0x7B:				//RRA  absolute, Y (+3)
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + ly;
		//operand = core_ror(core_get_mem(address), 1);
		ptr = core_get_mem(address);
		if ((psr & SR_FLAG_C)) ptr |= 0x100;
		if (ptr & 0x01) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		operand = (uint16)ptr >> 1;

		core_set_mem(address, operand);
		lpc += 3;
		lacc = core_add(lacc, operand, &psr);
		CPU_DEBUG("RRA");
		break;
	case 0x67:				//RRA
	case 0x77:
	case 0x6F:
	case 0x7F:
	case 0x63:
	case 0x73:
		switch (opcode & 0x1c) {
		case 0x00:		//(indirect, X)  (+2)
			address = (opcodes[1] + (uint16)lx) & 0xFF;
			address = core_get_word(address);
			//operand = core_ror(core_get_mem(core_get_word(address)), 1);
			ptr = core_get_mem(address);
			if ((psr & SR_FLAG_C)) ptr |= 0x100;
			if (ptr & 0x01) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			operand = (uint16)ptr >> 1;

			core_set_mem(core_get_word(address), operand);
			lpc += 2;
			break;
		case 0x10:		//(indirect), Y  (+2)
			address = (core_get_word(opcodes[1])) + (uint16)ly;
			//operand = core_ror(core_get_mem(address), 1);
			ptr = core_get_mem(address);
			if ((psr & SR_FLAG_C)) ptr |= 0x100;
			if (ptr & 0x01) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			operand = (uint16)ptr >> 1;
			core_set_mem(address, operand);
			lpc += 2;
			break;
		case 0x04:		//zeropage  (+2)
			address = opcodes[1];
			//operand = core_ror(core_get_mem(opcodes[1]), 1);
			ptr = core_get_mem(address);
			if ((psr & SR_FLAG_C)) ptr |= 0x100;
			if (ptr & 0x01) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			operand = (uint16)ptr >> 1;
			core_set_mem(address, operand);
			lpc += 2;
			break;
		case 0x0c:		//absolute (+3)
			address = ((uint16)opcodes[2] * 256) + opcodes[1];
			//operand = core_ror(core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), 1);
			ptr = core_get_mem(address);
			if ((psr & SR_FLAG_C)) ptr |= 0x100;
			if (ptr & 0x01) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			operand = (uint16)ptr >> 1;
			core_set_mem(address, operand);
			lpc += 3;
			break;
		case 0x14:		//zeropage, X  (+2)
			address = (opcodes[1] + lx) & 0xFF;
			//operand = core_ror(core_get_mem(address), 1);
			ptr = core_get_mem(address);
			if ((psr & SR_FLAG_C)) ptr |= 0x100;
			if (ptr & 0x01) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			operand = (uint16)ptr >> 1;
			core_set_mem(address, operand);
			lpc += 2;
			break;
		case 0x1c:		//absolute, X (+3)
			address = (((uint16)opcodes[2] * 256) + opcodes[1]) + lx;
			//operand = core_ror(core_get_mem(address), 1);
			ptr = core_get_mem(address);
			if ((psr & SR_FLAG_C)) ptr |= 0x100;
			if (ptr & 0x01) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			operand = (uint16)ptr >> 1;
			core_set_mem(address, operand);
			lpc += 3;
			break;
		}
		lacc = core_add(lacc, operand, &psr);
		CPU_DEBUG("RRA");
		break;
	case 0x5B:				//SRE  absolute, Y (+3)
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + ly;
		operand = core_get_mem(address);
		//operand = core_lsr(core_get_mem(address), 1);
		if (operand & 0x01) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		operand = (uint16)operand >> 1;
		//core_set_mem(address, operand);
		lpc += 3;
		lacc = core_xor(lacc, operand);

		CPU_DEBUG("SRE");
		break;
	case 0x47:				//SRE
	case 0x57:
	case 0x4F:
	case 0x5F:
	case 0x43:
	case 0x53:
		switch (opcode & 0x1c) {
		case 0x00:		//(indirect, X)  (+2)
			address = (opcodes[1] + (uint16)lx) & 0xFF;
			address = core_get_word(address);
			operand = core_get_mem(address);
			//operand = core_lsr(core_get_mem(core_get_word(address)), 1);
			if (operand & 0x01) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			operand = (uint16)operand >> 1;
			core_set_mem(address, operand);
			lpc += 2;
			break;
		case 0x10:		//(indirect), Y  (+2)
			address = (core_get_word(opcodes[1])) + (uint16)ly;
			operand = core_get_mem(address);
			//operand = core_lsr(core_get_mem(address), 1);
			if (operand & 0x01) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			operand = (uint16)operand >> 1;
			core_set_mem(address, operand);
			lpc += 2;
			break;
		case 0x04:		//zeropage  (+2)
			address = opcodes[1];
			operand = core_get_mem(address);
			//operand = core_lsr(core_get_mem(opcodes[1]), 1);
			if (operand & 0x01) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			operand = (uint16)operand >> 1;
			core_set_mem(address, operand);
			lpc += 2;
			break;
		case 0x0c:		//absolute (+3)
			address = ((uint16)opcodes[2] * 256) + opcodes[1];
			operand = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
			//operand = core_lsr(core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), 1);
			if (operand & 0x01) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			operand = (uint16)operand >> 1;
			core_set_mem(address, operand);
			lpc += 3;
			break;
		case 0x14:		//zeropage, X  (+2)
			address = (opcodes[1] + lx) & 0xFF;
			operand = core_get_mem(address);
			//operand = core_lsr(core_get_mem(address), 1);
			if (operand & 0x01) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			operand = (uint16)operand >> 1;
			core_set_mem(address, operand);
			lpc += 2;
			break;
		case 0x1c:		//absolute, X (+3)
			address = (((uint16)opcodes[2] * 256) + opcodes[1]) + lx;
			operand = core_get_mem(address);
			//operand = core_lsr(core_get_mem(address), 1);
			if (operand & 0x01) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			operand = (uint16)operand >> 1;
			core_set_mem(address, operand);
			lpc += 3;
			break;
		}
		lacc = core_xor(lacc, operand);
		CPU_DEBUG("SRE");
		break;
	case 0x9B:			//TAS
		address = ((uint16)opcodes[2] * 256) + opcodes[1];
		lsp = lacc & lx;
		operand = core_get_word(address) >> 8;
		core_set_mem(address, operand & lacc & lx);
		lpc += 3;
		CPU_DEBUG("TAS");
		break;
	case 0xEB:				//USBC  (SBC+NOP)
		lacc = core_sub(lacc, opcodes[1], &psr);
		lpc += 2;
		CPU_DEBUG("USBC");
		break;
	case 0xFB:			//ISB
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + ly;
		operand = core_get_mem(address);
		core_set_mem(address, operand + 1);
		lpc += 3;
		lacc = core_sub(lacc, operand + 1, &psr);
		CPU_DEBUG("ISB");
		goto skip_flag_test;
		//break;
	case 0xE7:			//ISB
	case 0xF7:
	case 0xEF:
	case 0xFF:
	case 0xE3:
	case 0xF3:
		switch (opcode & 0x1C) {
		case 0x00:		//(indirect, X)  (+2)
			address = (opcodes[1] + (uint16)lx) & 0xFF;
			operand = core_get_mem(core_get_word(address));
			core_set_mem(core_get_word(address), operand + 1);
			lpc += 2;
			break;
		case 0x10:		//(indirect), Y  (+2)
			address = (core_get_word(opcodes[1])) + (uint16)ly;
			operand = core_get_mem(address);
			core_set_mem(address, operand + 1);
			lpc += 2;
			break;
		case 0x04:		//zeropage  (+2)
			operand = core_get_mem(opcodes[1]);
			core_set_mem(opcodes[1], operand + 1);
			lpc += 2;
			break;
		case 0x0c:		//absolute (+3)
			operand = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
			core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand + 1);
			lpc += 3;
			break;
		case 0x14:		//zeropage, X  (+2)
			address = (opcodes[1] + lx) & 0xFF;
			operand = core_get_mem(address);
			core_set_mem(address, operand + 1);
			lpc += 2;
			break;
		case 0x1c:		//absolute, X (+3)
			address = (((uint16)opcodes[2] * 256) + opcodes[1]) + lx;
			operand = core_get_mem(address);
			core_set_mem(address, operand + 1);
			lpc += 3;
			break;
		}
		lacc = core_sub(lacc, operand + 1, &psr);
		CPU_DEBUG("ISB");
		goto skip_flag_test;
		//break;
	case 0x1B:				//SLO  absolute, Y (+3)
		address = (((uint16)opcodes[2] * 256) + opcodes[1]) + ly;
		operand = core_get_mem(address);
		//operand = core_asl(core_get_mem(address), 1);
		ptr = (uint16)operand << 1;
		if (ptr & 0x100) psr |= SR_FLAG_C;
		else psr &= ~SR_FLAG_C;
		core_set_mem(address, ptr);
		lpc += 3;
		lacc = core_orl(lacc, operand);
		CPU_DEBUG("SLO");
		break;
	case 0x07:				//SLO
	case 0x17:
	case 0x0F:
	case 0x1F:
	case 0x03:
	case 0x13:
		switch (opcode & 0x1c) {
		case 0x00:		//(indirect, X)  (+2)
			address = (opcodes[1] + (uint16)lx) & 0xFF;
			address = core_get_word(address);
			//operand = core_asl(core_get_mem(core_get_word(address)), 1);
			operand = core_get_mem(address);
			ptr = (uint16)operand << 1;
			if (ptr & 0x100) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			core_set_mem(address, ptr);
			//core_set_mem(core_get_word(address), operand);
			lpc += 2;
			break;
		case 0x10:		//(indirect), Y  (+2)
			address = (core_get_word(opcodes[1])) + (uint16)ly;
			//operand = core_asl(core_get_mem(address), 1);
			operand = core_get_mem(address);
			ptr = (uint16)operand << 1;
			if (ptr & 0x100) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			core_set_mem(address, ptr);
			//core_set_mem(address, operand);
			lpc += 2;
			break;
		case 0x04:		//zeropage  (+2)
			address = opcodes[1];
			//operand = core_asl(core_get_mem(opcodes[1]), 1);
			operand = core_get_mem(address);
			ptr = (uint16)operand << 1;
			if (ptr & 0x100) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			core_set_mem(address, ptr);
			//core_set_mem(opcodes[1], operand);
			lpc += 2;
			break;
		case 0x0c:		//absolute (+3)
			address = ((uint16)opcodes[2] * 256) + opcodes[1];
			//operand = core_asl(core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), 1);
			operand = core_get_mem(address);
			ptr = (uint16)operand << 1;
			if (ptr & 0x100) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			core_set_mem(address, ptr);
			//core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
			lpc += 3;
			break;
		case 0x14:		//zeropage, X  (+2)
			address = (opcodes[1] + lx) & 0xFF;
			//operand = core_asl(core_get_mem(address), 1);
			operand = core_get_mem(address);
			ptr = (uint16)operand << 1;
			if (ptr & 0x100) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			core_set_mem(address, ptr);
			//core_set_mem(address, operand);
			lpc += 2;
			break;
		case 0x1c:		//absolute, X (+3)
			address = (((uint16)opcodes[2] * 256) + opcodes[1]) + lx;
			//operand = core_asl(core_get_mem(address), 1);
			operand = core_get_mem(address);
			ptr = (uint16)operand << 1;
			if (ptr & 0x100) psr |= SR_FLAG_C;
			else psr &= ~SR_FLAG_C;
			core_set_mem(address, ptr);
			//core_set_mem(address, operand);
			lpc += 3;
			break;
		}
		lacc = core_orl(lacc, operand);
		CPU_DEBUG("SLO");
		break;
	}
	//check accumulator
	if (lacc == 0) psr |= SR_FLAG_Z;
	else psr &= ~SR_FLAG_Z;
	if (lacc & 0x80) psr |= SR_FLAG_N;
	else psr &= ~SR_FLAG_N;
skip_flag_test:
	psr |= 0x20;							//ignore bit always one

	_sr = psr;
	_x = lx;
	_y = ly;
	_acc = lacc;
	_pc = lpc;
	_sp = lsp;
	return;
}

int ins_counter = 0;

uint8 _mmc_cr = 0;
nes_mmc1 _mmc1_ctx;

#define SHIFT_REGISTER(x, y, z)		{ if(data & 0x80) { x=0x10; y=0; } else { y++; x = ((x >> 1) | ((data & 0x01) << 4)) & 0x1f; } if (y == 5) { z; y=0; } }


void prg_switch(nes_mmc1* ctx) {
	int index;
	uchar* ptr_buf;
	index = ((ctx->prg & 0x0F) << 14);
	//index = ctx->bank_table[(ctx->prg & 0x0F)];
	switch ((ctx->cr >> 2) & 0x03) {
	case 0:
	case 1:			//32 bit bank
		memcpy(_sram + 0x8000, _mmc.rom + index, 0x8000);
		break;
	case 2:
		memcpy(_sram + 0xC000, _mmc.rom + index, 0x4000);
		break;
	case 3:
		ptr_buf = _mmc.rom + index;
		memcpy(_sram + 0x8000, ptr_buf, 0x4000);
		break;
	}
}

void mmc1_write(void * payload, uint16 address, uint8 data) {
	int index;
	nes_mmc1 * ctx = (nes_mmc1 *)payload;
	switch (address & 0xE000) {
	case 0x8000:		//control
		SHIFT_REGISTER(ctx->cr, ctx->cr_shift, 
			_mmc_cr = ctx->cr
		)
		break;	
	case 0xA000:		//CHR bank 0
		SHIFT_REGISTER(ctx->ch0, ctx->cr_shift,
		if (ctx->cr & 0x10) {
			ppu_set_ram(0, _mmc.chrom + ((ctx->ch0 & 0x1F) << 12), 0x1000);
		}
		else {
			ppu_set_ram(0, _mmc.chrom + ((ctx->ch0 & 0x1E) << 12), 0x2000);
		})
		
		break;
	case 0xC000:		//CHR bank 1

		SHIFT_REGISTER(ctx->ch1, ctx->cr_shift,
		if (ctx->cr & 0x10) {
			ppu_set_ram(0x1000, _mmc.chrom + ((ctx->ch1 & 0x1F) << 12), 0x1000);
		}
		else {
			ppu_set_ram(0, _mmc.chrom + ((ctx->ch1 & 0x1E) << 12), 0x2000);
		})
		
		break;
	case 0xE000:		//PRG bank
		SHIFT_REGISTER(ctx->prg, ctx->cr_shift,
			prg_switch(ctx);
		)
		
		break;
	}
}

void core_config(uint8 num_banks, uint8 mapper, uchar* rom, int len, uint8 ch_bank, uchar * chrom, int chlen) {
	uint start = 0x8000;
	uint8 i;
	_mmc.rom = rom;
	_mmc.size = len;
	_mmc.chrom = chrom;
	_mmc.chsize = chlen;
	switch (mapper) {
	case 0:						//no mapper
		switch (num_banks) {			//number of banks for vrom
		case 1:
			start = 0xc000;
			len = 0x4000;			//16KB
			break;
		case 2:
			start = 0x8000;
			len = 0x8000;			//32KB
			break;
		}
		memcpy(_sram + start, rom, len);
		ppu_set_ram(0, chrom, 0x2000);
		break;
	case 1:						//MMC1
		memset(&_mmc1_ctx, 0, sizeof(_mmc1_ctx));
		memcpy(_sram + 0x8000, rom + (num_banks * 0x4000) - 0x8000, 0x8000);
		_mmc1_ctx.bank_table[0] = (num_banks * 0x4000) - 0x8000;
		_mmc1_ctx.bank_table[1] = (num_banks * 0x4000) - 0x4000;		
		for (i = 2; i < num_banks; i++) {
			_mmc1_ctx.bank_table[i] = (unsigned)(i - 2) * 0x4000;
		}
		ppu_set_ram(0, chrom, 0x2000);
		_mmc.payload = &_mmc1_ctx;
		_mmc.write = mmc1_write;
		break;
	}
}

void core_init(uchar* buffer, int len) {
	uint8 num_banks = buffer[4];
	uint16 start;
	uint8 mapper;
	if (buffer[9] & 0x01) {
		//system PAL
	}
	else {
		//system NTSC
	}
	ins_counter = 0;
	mapper = (buffer[7] & 0xF0) | ((buffer[6] >> 4) & 0x0F);
	core_config(num_banks, mapper, buffer + 0x10, len - 0x10, buffer[5], buffer + 0x10 + (num_banks * 0x4000), buffer[5]* 0x2000);
	ppu_init(buffer[6]);
	start = core_get_word(0xFFFC);
	_pc = start;			//set pc to start of cartridge ROM
}

uchar core_exec(uchar* vbuffer) {
	//printf("A:%02X X:%02X Y:%02X P:%02X SP:%02X PC:%04X [00h]:%02X [10h]:%02X [11h]:%02X\r\n", _acc, _x, _y, _sr, _sp, _pc, _sram[0], _sram[0x10], _sram[0x11]);
	int ret = 0;
	if (ppu_get_vblank())  {
		//start nmi
		if ((_sr & SR_FLAG_I)) {
			_stack[_sp--] = _pc >> 8;			//PCH
			_stack[_sp--] = _pc;				//PCL
			_stack[_sp--] = _sr;					//SR
			_pc = core_get_word(0xFFFA);
			ins_counter = 0;
		}
		ppu_set_vblank(0);
	}
	core_decode(_sram + (unsigned)_pc);
	
	ins_counter++;
	if ((ins_counter % 7501) == 0) {
		ppu_set_vblank(1);
	}
	if ((ins_counter % 4057) == 0) {
		ppu_render(vbuffer);
		//ppu_set_vblank(1);
		ret = 1;
	}
	if (_pc == 0xb4ac) {
		_pc = _pc;
	}
	if (_pc == 0xe45b) {
		_pc = _pc;
	}
	if (_pc < 4) {
		getchar();
	}
	if (_pc == 0x0004) {
		printf("executed : %ld\r\n", ins_counter);
		getchar();
	}
	return ret;
	//getchar();
}