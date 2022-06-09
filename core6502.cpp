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



uchar core_orl(uchar a, uchar operand) {
	//check D flag for BCD operation, C flag for carry operation, set C if needed
	uint16 ret;
	ret = a | operand;
	return ret;
}

uchar core_and(uchar a, uchar operand) {
	//check D flag for BCD operation, C flag for carry operation, set C if needed
	uint16 ret;
	ret = a & operand;
	return ret;
}

uchar core_xor(uchar a, uchar operand) {
	//check D flag for BCD operation, C flag for carry operation, set C if needed
	uint16 ret;
	ret = a ^ operand;
	return ret;
}

uchar core_lda(uchar a, uchar operand) {
	//check D flag for BCD operation, C flag for carry operation, set C if needed
	uint16 ret;
	ret = operand;
	return ret;
}

uchar core_asl(uchar a, uchar l) {
	uint16 ret;
	ret = (uint16)a << l;
	if (ret & 0x100) _sr |= SR_FLAG_C;
	else _sr &= ~SR_FLAG_C;
	return ret;
}

uchar core_lsr(uchar a, uchar l) {
	uint16 ret;
	if (a & 0x01) _sr |= SR_FLAG_C;
	else _sr &= ~SR_FLAG_C;
	ret = (uint16)a >> l;
	return ret;
}

uchar core_rol(uchar a, uchar l) {
	uint16 ret;
	ret = (uint16)a << l;
	ret |= (_sr & SR_FLAG_C);
	if (ret & 0x100) _sr |= SR_FLAG_C;
	else _sr &= ~SR_FLAG_C;
	return ret;
}

uchar core_ror(uchar a, uchar l) {
	uint16 ret;
	uint8 csr = (_sr & SR_FLAG_C);
	if (a & 0x01) _sr |= SR_FLAG_C;
	else _sr &= ~SR_FLAG_C;
	ret = (uint16)a >> l;
	if (csr) ret |= 0x80;
	return ret;
}

uchar core_cmp(uchar a, uchar operand) {
	//check D flag for BCD operation, C flag for carry operation, set C if needed
	uint16 ret = 0;
	if (a == operand) {
		_sr |= SR_FLAG_Z;
		_sr |= SR_FLAG_C;
		_sr &= ~SR_FLAG_N;
	}
	else if (a > operand) {
		_sr &= ~SR_FLAG_Z;
		_sr |= SR_FLAG_C;
		if((a- operand) & 0x80) _sr |= SR_FLAG_N;
		else _sr &= ~SR_FLAG_N;
	}
	else if (a < operand) {
		_sr &= ~SR_FLAG_Z;
		_sr &= ~SR_FLAG_C;
		if ((a - operand) & 0x80) _sr |= SR_FLAG_N;
		else _sr &= ~SR_FLAG_N;
	}
	return ret;
}

uchar add_is_overflow(int8 lhs, int8 rhs) {
	if (lhs > 0 && rhs > 0 && (rhs > (INT8_MAX - lhs))) return 1;
	if (lhs < 0 && rhs < 0 && (lhs < (INT8_MIN - rhs))) return 1;
	return 0;
}

uchar sub_is_overflow(int8 lhs, int8 rhs) {
	int8 diff = lhs - rhs;
	//printf("diff:%x, lhs:%x, rhs:%x\n", diff, lhs, rhs);
	if (rhs >= 0 && diff > lhs)return 1;
	if (rhs < 0 && diff < lhs)return 1;
	//printf("return 0;\n");
	return 0;
}

uchar core_add(uchar a, uchar operand) {
	//check D flag for BCD operation, C flag for carry operation, set C if needed
	uchar nb = 0;
	uchar bcr = 0;
	uint16 ret;
	if (add_is_overflow(a, operand)) _sr |= SR_FLAG_V;
	else _sr &= ~SR_FLAG_V;
	//if (_sr & SR_FLAG_D) {
	if(0) {
		nb = ((a & 0x0F) + (operand & 0x0F) + (_sr & SR_FLAG_C));
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
		ret = a + operand + (_sr & SR_FLAG_C);
		if (ret > 255) bcr = 1;
	}
	if (bcr) _sr |= SR_FLAG_C;
	else _sr &= ~SR_FLAG_C;
	return ret;
}

uchar core_sub(int8 a, int8 operand) {
	uchar nb = 0;
	uchar op1;
	uchar temp;
	uint16 res;
	int16 ires;
	uchar carry = 0;
	uchar bcr = 0;
	int16 ret = 0;
	uchar a_0 = a / 16;
	uchar a_1 = (a & 0x0F) % 10;
	uchar o_0 = operand / 16;
	uchar o_1 = (operand & 0x0F) % 10;
	if (sub_is_overflow(a, operand)) _sr |= SR_FLAG_V;
	else _sr &= ~SR_FLAG_V;
	//if (_sr & SR_FLAG_D) {
	if(0) {
		if (a_1 < (o_1 + (_sr & SR_FLAG_C))) {
			nb = a_1 + 10 - (o_1 + (_sr & SR_FLAG_C));
			bcr = 1;
		}
		else {
			nb = a_1 - (o_1 + (_sr & SR_FLAG_C));
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
		carry = !(_sr & SR_FLAG_C);
		op1 = _acc;
		res = (int16)((int8)op1 - carry) - (int16)operand;
		ires = ((uchar)op1 - (uchar)carry) - (uchar)operand;
		temp = ires;
		if (ires & 0x100) { _sr &= ~SR_FLAG_C; }
		else { _sr |= SR_FLAG_C; }
		//ret = a - operand - !(_sr & SR_FLAG_C);
		//if (ret > 255) bcr = 1;
		//if (ret < 0) bcr = 1;
	}
	//if (bcr) _sr &= ~SR_FLAG_C;
	//else _sr |= SR_FLAG_C;

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
	uint16 hh = 0;
	uchar ll = core_get_mem(address);
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


void core_decode(uchar * opcodes) {
	uchar opcode = opcodes[0];
	uchar operand = 0;
	uint16 address = 0;
	switch (opcode) {
		case 0x00:		//BRK
			_sr |= SR_FLAG_B;			//set break flag
			_stack[_sp--] = (_pc + 2) >> 8;			//PCH
			_stack[_sp--] = (_pc + 2);				//PCL
			_stack[_sp--] = _sr;					//SR
			CPU_DEBUG("BRK");
			//should jump to break interrupt vector (to do)
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x40:			//RTI		return from interrupt pull sr pull pc
			_sr = _stack[++_sp];
			_sr &= ~SR_FLAG_B;			//clear break flag
			opcode = _stack[++_sp];
			address = _stack[++_sp];
			_pc = (((uint16)address << 8) | opcode);
			CPU_DEBUG("RTI");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x20:			//JSR
			_stack[_sp--] = (_pc + 2) >> 8;			//PCH
			_stack[_sp--] = (_pc + 2);				//PCL
			_pc = ((uint16)opcodes[2] * 256) + opcodes[1];		//absolute addressing mode
			CPU_DEBUG("JSR");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x60:			//RTS			return from subroutine
			opcode = _stack[++_sp];
			address = _stack[++_sp];
			_pc = (((uint16)address << 8) | opcode) + 1;
			CPU_DEBUG("RTS");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x01:			//ORA Or with accumulator
		case 0x05:
		case 0x11:
		case 0x15:
		case 0x09:
		case 0x19:
		case 0x0d:
		case 0x1d:
			switch (opcode & 0x1c) {
			case 0x00:		//(indirect, X)  (+2)
				address = (opcodes[1] + (uint16)_x) & 0xFF;
				_acc = core_orl(_acc, core_get_mem( core_get_word(address)));
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				_acc = core_orl(_acc, core_get_mem(opcodes[1]));
				_pc += 2;
				break;
			case 0x08:		//immdt  (+2)
				_acc = core_orl(_acc, opcodes[1]);
				_pc += 2;
				break;
			case 0x0c:		//absolute (+3)
				_acc = core_orl(_acc, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]));
				_pc += 3;
				break;
			case 0x10:		//(indirect), Y  (+2)
				_acc = core_orl(_acc, core_get_mem((USE_CARRY + core_get_word(opcodes[1])) + (uint16)_y));
				_pc += 2;
				break;
			case 0x14:		//zeropage, X  (+2)
				address = (opcodes[1] + _x) & 0xFF;
				_acc = core_orl(_acc, core_get_mem(address));
				_pc += 2;
				break;
			case 0x18:		//absolute, Y (+3)
				_acc = core_orl(_acc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + _y ));
				_pc += 3;
				break;
			case 0x1c:		//absolute, X (+3)
				_acc = core_orl(_acc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + _x ));
				_pc += 3;
				break;
			}
			CPU_DEBUG("ORA");
			break;
		case 0x0a:				//ASL arithmetic shift left
			_acc = core_asl(_acc, 1);
			_pc += 1;
			CPU_DEBUG("ASL");
			break;
		case 0x06:					//ASL arithmetic shift left
		case 0x0e:
		case 0x16:
		case 0x1e:
			switch (opcode & 0x18) {
			case 0x00:		//zeropage  (+2)
				operand = core_asl(core_get_mem(opcodes[1]), 1);
				core_set_mem(opcodes[1], operand);
				_pc += 2;
				break;
			case 0x08:		//absolute (+3)
				operand = core_asl(core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), 1);
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
				_pc += 3;
				break;
			case 0x10:		//zeropage, X  (+2)
				address = (opcodes[1] + _x) & 0xFF;
				operand = core_asl(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 2;
				break;
			case 0x18:		//absolute, X (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _x ;
				operand = core_asl(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 3;
				break;
			}
			CPU_DEBUG("ASL");
			break;
		case 0x08:			//PHP			push status register
			_stack[_sp--] = _sr;
			_pc += 1;
			CPU_DEBUG("PHP");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x28:			//PLP			pull status register		10
			_sr = _stack[++_sp];
			_sr &= ~SR_FLAG_B;
			_pc += 1;
			CPU_DEBUG("PLP");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x48:			//PHA			push accumulator
			_stack[_sp--] = _acc;
			_pc += 1;
			CPU_DEBUG("PHA");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x68:			//PLA			pull accumulator
			_acc = _stack[++_sp];
			_pc += 1;
			CPU_DEBUG("PLA");
			break;
		case 0x18:			//CLC
			_sr &= ~SR_FLAG_C;
			_pc += 1;
			CPU_DEBUG("CLC");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x58:			//CLI
			_sr &= ~SR_FLAG_I;
			_pc += 1;
			CPU_DEBUG("CLI");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xB8:			//CLV
			_sr &= ~SR_FLAG_V;
			_pc += 1;
			CPU_DEBUG("CLV");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xD8:			//CLD
			_sr &= ~SR_FLAG_D;
			_pc += 1;
			CPU_DEBUG("CLD");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x24:			//BIT
		case 0x2c:
			switch (opcode & 0x08) {
			case 0x00:		//zeropage
				operand = core_get_mem(opcodes[1]);
				if (operand & 0x80) _sr |= SR_FLAG_N;
				else _sr &= ~SR_FLAG_N;
				if (operand & 0x40) _sr |= SR_FLAG_V;
				else _sr &= ~SR_FLAG_V;
				if (core_and(_acc, operand) == 0) _sr |= SR_FLAG_Z;
				else _sr &= ~SR_FLAG_Z;
				_pc += 2;
				break;
			case 0x08:		//absolute
				operand = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
				if (operand & 0x80) _sr |= SR_FLAG_N;
				else _sr &= ~SR_FLAG_N;
				if (operand & 0x40) _sr |= SR_FLAG_V;
				else _sr &= ~SR_FLAG_V;
				if (core_and(_acc, operand) == 0) _sr |= SR_FLAG_Z;
				else _sr &= ~SR_FLAG_Z;
				_pc += 3;
				break;
			}
			CPU_DEBUG("BIT");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x21:			//AND
		case 0x25:
		case 0x35:
		case 0x31:
		case 0x29:
		case 0x39:
		case 0x2d:
		case 0x3d:
			switch (opcode & 0x1c) {
			case 0x00:		//(indirect, X)  (+2)
				address = (opcodes[1] + (uint16)_x) & 0xFF;
				_acc = core_and(_acc, core_get_mem(USE_CARRY + core_get_word(address)));
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				_acc = core_and(_acc, core_get_mem(opcodes[1]));
				_pc += 2;
				break;
			case 0x08:		//immdt  (+2)
				_acc = core_and(_acc, opcodes[1]);
				_pc += 2;
				break;
			case 0x0c:		//absolute (+3)
				_acc = core_and(_acc, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]));
				_pc += 3;
				break;
			case 0x10:		//(indirect), Y  (+2)
				_acc = core_and(_acc, core_get_mem((USE_CARRY + core_get_word(opcodes[1])) + (uint16)_y));
				_pc += 2;
				break;
			case 0x14:		//zeropage, X  (+2)
				_acc = core_and(_acc, core_get_mem((opcodes[1] + _x) & 0xFF));
				_pc += 2;
				break;
			case 0x18:		//absolute, Y (+3)
				_acc = core_and(_acc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + _y));
				_pc += 3;
				break;
			case 0x1c:		//absolute, X (+3)
				_acc = core_and(_acc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + _x));
				_pc += 3;
				break;
			}
			CPU_DEBUG("AND");
			break;
		case 0x2a:			//ROL accumulator
			_acc = core_rol(_acc, 1);
			_pc += 1;
			CPU_DEBUG("ROL");
			break;
		case 0x26:			//ROL
		case 0x2e:
		case 0x36:
		case 0x3e:
			switch (opcode & 0x18) {
			case 0x00:		//zeropage  (+2)
				operand = core_rol(core_get_mem(opcodes[1]), 1);
				core_set_mem(opcodes[1], operand);
				_pc += 2;
				break;
			case 0x08:		//absolute (+3)
				operand = core_rol(core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), 1);
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
				_pc += 3;
				break;
			case 0x10:		//zeropage, X  (+2)
				address = (opcodes[1] + _x) & 0xFF;
				operand = core_rol(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 2;
				break;
			case 0x18:		//absolute, X (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _x ;
				operand = core_rol(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 3;
				break;
			}
			CPU_DEBUG("ROL");
			break;
		case 0x4c:			//JMP
		case 0x6c:
			switch (opcode & 0x20) {
			case 0x00:			//absolute
				_pc = ((uint16)opcodes[2] * 256) + opcodes[1];
				break;
			case 0x20:			//indirect
				if (opcodes[1] != 0xFF) {
					_pc = core_get_word(((uint16)opcodes[2] * 256) + opcodes[1]);
				}
				else {
					operand = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
					address = core_get_mem(((uint16)opcodes[2] * 256) );
					address = (address << 8) | operand;
					_pc = address;
				}
				break;
			}
			CPU_DEBUG("JMP");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x41:			//EOR
		case 0x51:
		case 0x45:
		case 0x55:
		case 0x49:
		case 0x4d:
		case 0x59:
		case 0x5d:
			switch (opcode & 0x1c) {
			case 0x00:		//(indirect, X)  (+2)
				address = (opcodes[1] + (uint16)_x) & 0xFF;
				_acc = core_xor(_acc, core_get_mem(USE_CARRY + core_get_word(address)));
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				_acc = core_xor(_acc, core_get_mem(opcodes[1]));
				_pc += 2;
				break;
			case 0x08:		//immdt  (+2)
				_acc = core_xor(_acc, opcodes[1]);
				_pc += 2;
				break;
			case 0x0c:		//absolute (+3)
				_acc = core_xor(_acc, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]));
				_pc += 3;
				break;
			case 0x10:		//(indirect), Y  (+2)
				_acc = core_xor(_acc, core_get_mem((USE_CARRY + core_get_word(opcodes[1])) + (uint16)_y));
				_pc += 2;
				break;
			case 0x14:		//zeropage, X  (+2)
				_acc = core_xor(_acc, core_get_mem((opcodes[1] + _x) & 0xFF));
				_pc += 2;
				break;
			case 0x18:		//absolute, Y (+3)
				_acc = core_xor(_acc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + _y));
				_pc += 3;
				break;
			case 0x1c:		//absolute, X (+3)
				_acc = core_xor(_acc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + _x));
				_pc += 3;
				break;
			}
			CPU_DEBUG("EOR");
			break;
		case 0x4a:			//LSR accumulator
			_acc = core_lsr(_acc, 1);
			_pc += 1;
			CPU_DEBUG("LSR");
			break;
		case 0x46:			//LSR
		case 0x4e:
		case 0x56:
		case 0x5e:
			switch (opcode & 0x18) {
			case 0x00:		//zeropage  (+2)
				operand = core_lsr(core_get_mem(opcodes[1]), 1);
				core_set_mem(opcodes[1], operand);
				_pc += 2;
				break;
			case 0x08:		//absolute (+3)
				operand = core_lsr(core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), 1);
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
				_pc += 3;
				break;
			case 0x10:		//zeropage, X  (+2)
				address = (opcodes[1] + _x) & 0xFF;
				operand = core_lsr(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 2;
				break;
			case 0x18:		//absolute, X (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _x ;
				operand = core_lsr(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 3;
				break;
			}
			CPU_DEBUG("LSR");
			break;
		case 0x61:			//ADC		22
		case 0x65:
		case 0x69:
		case 0x6d:
		case 0x71:
		case 0x75:
		case 0x79:
		case 0x7d:
			switch (opcode & 0x1c) {
				case 0x00:		//(indirect, X)  (+2)
					address = (opcodes[1] + (uint16)_x) & 0xFF;
					_acc = core_add(_acc, core_get_mem(USE_CARRY + core_get_word(address)));
					_pc += 2;
					break;
				case 0x04:		//zeropage  (+2)
					_acc = core_add(_acc, core_get_mem(opcodes[1]));
					_pc += 2;
					break;
				case 0x08:		//immdt  (+2)
					_acc = core_add(_acc, opcodes[1]);
					_pc += 2;
					break;
				case 0x0c:		//absolute (+3)
					_acc = core_add(_acc, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]));
					_pc += 3;
					break;
				case 0x10:		//(indirect), Y  (+2)
					_acc = core_add(_acc, core_get_mem((USE_CARRY + core_get_word(opcodes[1])) +(uint16)_y));
					_pc += 2;
					break;
				case 0x14:		//zeropage, X  (+2)
					_acc = core_add(_acc, core_get_mem((opcodes[1] + _x) & 0xFF));
					_pc += 2;
					break;
				case 0x18:		//absolute, Y (+3)
					_acc = core_add(_acc, core_get_mem( (((uint16)opcodes[2] * 256) + opcodes[1]) + _y ));
					_pc += 3;
					break;
				case 0x1c:		//absolute, X (+3)
					_acc = core_add(_acc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + _x ));
					_pc += 3;
					break;
			}
			CPU_DEBUG("ADC");
			break;
		case 0x6a:			//ROR accumulator
			_acc = core_ror(_acc, 1);
			_pc += 1;
			CPU_DEBUG("ROR");
			break;
		case 0x66:			//ROR
		case 0x6e:
		case 0x76:
		case 0x7e:
			switch (opcode & 0x18) {
			case 0x00:		//zeropage  (+2)
				operand = core_ror(core_get_mem(opcodes[1]), 1);
				core_set_mem(opcodes[1], operand);
				_pc += 2;
				break;
			case 0x08:		//absolute (+3)
				operand = core_ror(core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), 1);
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
				_pc += 3;
				break;
			case 0x10:		//zeropage, X  (+2)
				address = (opcodes[1] + _x) & 0xFF;
				operand = core_ror(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 2;
				break;
			case 0x18:		//absolute, X (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _x  ;
				operand = core_ror(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 3;
				break;
			}
			CPU_DEBUG("ROR");
			break;
		case 0x81:			//STA
		case 0x85:
		case 0x8d:
		case 0x91:
		case 0x95:
		case 0x99:
		case 0x9d:
			switch (opcode & 0x1c) {
			case 0x00:		//(indirect, X)  (+2)
				address = (opcodes[1] + (uint16)_x) & 0xFF;
				core_set_mem(USE_CARRY + core_get_word(address), _acc);
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				core_set_mem(opcodes[1], _acc);
				_pc += 2;
				break;
			case 0x0c:		//absolute (+3)
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], _acc);
				_pc += 3;
				break;
			case 0x10:		//(indirect), Y  (+2)
				core_set_mem((USE_CARRY + core_get_word(opcodes[1])) + (uint16)_y, _acc);
				_pc += 2;
				break;
			case 0x14:		//zeropage, X  (+2)
				core_set_mem((opcodes[1] + _x) & 0xFF, _acc);
				_pc += 2;
				break;
			case 0x18:		//absolute, Y (+3)
				core_set_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + _y, _acc);
				_pc += 3;
				break;
			case 0x1c:		//absolute, X (+3)
				core_set_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + _x, _acc);
				_pc += 3;
				break;
			}
			CPU_DEBUG("STA");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		case 0x84:			//STY
		case 0x8c:
		case 0x94:
			switch (opcode & 0x18) {
			case 0x00:		//zeropage  (+2)
				core_set_mem(opcodes[1], _y);
				_pc += 2;
				break;
			case 0x08:		//absolute (+3)
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], _y);
				_pc += 3;
				break;
			case 0x10:		//zeropage, X  (+2)
				core_set_mem((opcodes[1] + _x) & 0xFF, _y);
				_pc += 2;
				break;
			}
			CPU_DEBUG("STY");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		case 0x86:			//STX
		case 0x8e:
		case 0x96:
			switch (opcode & 0x18) {
			case 0x00:		//zeropage  (+2)
				core_set_mem(opcodes[1], _x);
				_pc += 2;
				break;
			case 0x08:		//absolute (+3)
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], _x);
				_pc += 3;
				break;
			case 0x10:		//zeropage, Y  (+2)
				core_set_mem((opcodes[1] + _y) & 0xFF, _x);
				_pc += 2;
				break;
			}
			CPU_DEBUG("STX");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		case 0xA0:			//LDY
			_y = opcodes[1];
			_pc += 2;
			if (_y == 0) _sr |= SR_FLAG_Z;
			else _sr &= ~SR_FLAG_Z;
			if (_y & 0x80) _sr |= SR_FLAG_N;
			else _sr &= ~SR_FLAG_N;
			CPU_DEBUG("LDY");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xA4:			//LDY
		case 0xAc:
		case 0xB4:
		case 0xBc:
			switch (opcode & 0x18) {
			case 0x00:		//zeropage  (+2)
				_y = core_get_mem(opcodes[1]);
				_pc += 2;
				break;
			case 0x08:		//absolute (+3)
				_y = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
				_pc += 3;
				break;
			case 0x10:		//zeropage, X  (+2)
				_y = core_get_mem((opcodes[1] + _x) & 0xFF);
				_pc += 2;
				break;
			case 0x18:		//absolute, X (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _x ;
				_y = core_get_mem(address);
				_pc += 3;
				break;
			}
			if (_y == 0) _sr |= SR_FLAG_Z;
			else _sr &= ~SR_FLAG_Z;
			if (_y & 0x80) _sr |= SR_FLAG_N;
			else _sr &= ~SR_FLAG_N;
			CPU_DEBUG("LDY");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xA1:			//LDA
		case 0xA5:
		case 0xA9:
		case 0xAd:
		case 0xB1:		
		case 0xB5:
		case 0xB9:
		case 0xBd:
			if (_pc == 0x86ff) {
				_pc = _pc;
			}
			switch (opcode & 0x1c) {
			case 0x00:		//(indirect, X)  (+2)
				address = (opcodes[1] + (uint16)_x) & 0xFF;
				_acc = core_lda(_acc, core_get_mem(USE_CARRY + core_get_word( address )));
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				address = opcodes[1];
				_acc = core_lda(_acc, core_get_mem(address));
				_pc += 2;
				break;
			case 0x08:		//immdt  (+2)
				_acc = core_lda(_acc, opcodes[1]);
				_pc += 2;
				break;
			case 0x0c:		//absolute (+3)
				address = ((uint16)opcodes[2] * 256) + opcodes[1];
				_acc = core_lda(_acc, core_get_mem(address));
				_pc += 3;
				break;
			case 0x10:		//(indirect), Y  (+2)
				address = (core_get_word(opcodes[1])) + (uint16)_y;
				_acc = core_lda(_acc, core_get_mem(address));
				_pc += 2;
				break;
			case 0x14:		//zeropage, X  (+2)
				address = (opcodes[1] + _x) & 0xFF;
				_acc = core_lda(_acc, core_get_mem(address));
				_pc += 2;
				break;
			case 0x18:		//absolute, Y (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _y;
				_acc = core_lda(_acc, core_get_mem(address));
				_pc += 3;
				break;
			case 0x1c:		//absolute, X (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _x;
				_acc = core_lda(_acc, core_get_mem(address ));
				_pc += 3;
				break;
			}
			CPU_DEBUG("LDA");
			break;
		case 0xA2:			//LDX
			_x = opcodes[1];
			_pc += 2;
			if (_x == 0) _sr |= SR_FLAG_Z;
			else _sr &= ~SR_FLAG_Z;
			if (_x & 0x80) _sr |= SR_FLAG_N;
			else _sr &= ~SR_FLAG_N;
			CPU_DEBUG("LDX");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
		case 0xA6:
		case 0xAe:
		case 0xB6:
		case 0xBe:
			switch (opcode & 0x18) {
			case 0x00:		//zeropage  (+2)
				_x = core_get_mem(opcodes[1]);
				_pc += 2;
				break;
			case 0x08:		//absolute (+3)
				_x = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
				_pc += 3;
				break;
			case 0x10:		//zeropage, Y  (+2)
				_x = core_get_mem((opcodes[1] + _y) & 0xFF);
				_pc += 2;
				break;
			case 0x18:		//absolute, Y (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _y ;
				_x = core_get_mem(address);
				_pc += 3;
				break;
			}
			if (_x == 0) _sr |= SR_FLAG_Z;
			else _sr &= ~SR_FLAG_Z;
			if (_x & 0x80) _sr |= SR_FLAG_N;
			else _sr &= ~SR_FLAG_N;
			CPU_DEBUG("LDX");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xA8:			//TAY		accumulator to y
			_y = _acc;
			_pc += 1;
			if (_y == 0) _sr |= SR_FLAG_Z;
			else _sr &= ~SR_FLAG_Z;
			if (_y & 0x80) _sr |= SR_FLAG_N;
			else _sr &= ~SR_FLAG_N;
			CPU_DEBUG("TAY");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xAA:			//TAX		accumulator to x
			_x = _acc;
			_pc += 1;
			if (_x == 0) _sr |= SR_FLAG_Z;
			else _sr &= ~SR_FLAG_Z;
			if (_x & 0x80) _sr |= SR_FLAG_N;
			else _sr &= ~SR_FLAG_N;
			CPU_DEBUG("TAX");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xBA:			//TSX		sp to x
			_x = _sp;
			_pc += 1;
			if (_x == 0) _sr |= SR_FLAG_Z;
			else _sr &= ~SR_FLAG_Z;
			if (_x & 0x80) _sr |= SR_FLAG_N;
			else _sr &= ~SR_FLAG_N;
			CPU_DEBUG("TSX");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x8A:			//TXA		x to accumulator
			_acc = _x;
			_pc += 1;
			CPU_DEBUG("TXA");
			break;
		case 0x98:			//TYA		y to accumulator
			_acc = _y;
			_pc += 1;
			CPU_DEBUG("TYA");
			break;
		case 0x9A:			//TXS		x to stack pointer
			_sp = _x;
			_pc += 1;
			CPU_DEBUG("TXS");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xC0:			//CPY
		case 0xC4:
		case 0xCc:
			switch (opcode & 0x0C) {
			case 0x00:		//immediate  (+2)
				core_cmp(_y, opcodes[1]);
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				core_cmp(_y, core_get_mem(opcodes[1]));
				_pc += 2;
				break;
			case 0x0C:		//absolute (+3)
				core_cmp(_y, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]));
				_pc += 3;
				break;
			}
			CPU_DEBUG("CPY");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xC1:			//CMP
		case 0xC5:
		case 0xC9:
		case 0xCd:
		case 0xd1:			
		case 0xd5:
		case 0xd9:
		case 0xdd:
			switch (opcode & 0x1c) {
			case 0x00:		//(indirect, X)  (+2)
				address = (opcodes[1] + (uint16)_x) & 0xFF;
				core_cmp(_acc, core_get_mem(USE_CARRY + core_get_word(address)));
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				core_cmp(_acc, core_get_mem(opcodes[1]));
				_pc += 2;
				break;
			case 0x08:		//immdt  (+2)
				core_cmp(_acc, opcodes[1]);
				_pc += 2;
				break;
			case 0x0c:		//absolute (+3)
				core_cmp(_acc, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]));
				_pc += 3;
				break;
			case 0x10:		//(indirect), Y  (+2)
				core_cmp(_acc, core_get_mem((USE_CARRY + core_get_word(opcodes[1])) + (uint16)_y));
				_pc += 2;
				break;
			case 0x14:		//zeropage, X  (+2)
				core_cmp(_acc, core_get_mem((opcodes[1] + _x) & 0xFF));
				_pc += 2;
				break;
			case 0x18:		//absolute, Y (+3)
				core_cmp(_acc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + _y ));
				_pc += 3;
				break;
			case 0x1c:		//absolute, X (+3)
				core_cmp(_acc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + _x ));
				_pc += 3;
				break;
			}
			CPU_DEBUG("CMP");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xC6:			//DEC
		case 0xCE:
		case 0xD6:
		case 0xDE:
			switch (opcode & 0x18) {
			case 0x00:		//zeropage  (+2)
				operand = core_get_mem(opcodes[1]);
				operand--;
				core_set_mem(opcodes[1], operand);
				_pc += 2;
				break;
			case 0x08:		//absolute (+3)
				operand = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
				operand--;
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
				_pc += 3;
				break;
			case 0x10:		//zeropage, X  (+2)
				address = (opcodes[1] + _x) & 0xFF;
				operand = core_get_mem(address);
				operand--;
				core_set_mem(address, operand);
				_pc += 2;
				break;
			case 0x18:		//absolute, X (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _x;
				operand = core_get_mem( address );
				operand--;
				core_set_mem(address , operand);
				_pc += 3;
				break;
			}
			if (operand == 0) _sr |= SR_FLAG_Z;
			else _sr &= ~SR_FLAG_Z;
			if (operand & 0x80) _sr |= SR_FLAG_N;
			else _sr &= ~SR_FLAG_N;
			CPU_DEBUG("DEC");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xC8:			//INY		increment y
			_y++;
			_pc += 1;
			if (_y == 0) _sr |= SR_FLAG_Z;
			else _sr &= ~SR_FLAG_Z;
			if (_y & 0x80) _sr |= SR_FLAG_N;
			else _sr &= ~SR_FLAG_N;
			CPU_DEBUG("INY");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xCA:			//DEX		decrement x
			_x--;
			_pc += 1;
			if (_x == 0) _sr |= SR_FLAG_Z;
			else _sr &= ~SR_FLAG_Z;
			if (_x & 0x80) _sr |= SR_FLAG_N;
			else _sr &= ~SR_FLAG_N;
			CPU_DEBUG("DEX");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x88:			//DEY			30
			_y--;
			_pc += 1;
			if (_y == 0) _sr |= SR_FLAG_Z;
			else _sr &= ~SR_FLAG_Z;
			if (_y & 0x80) _sr |= SR_FLAG_N;
			else _sr &= ~SR_FLAG_N;
			CPU_DEBUG("DEY");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xe8:			//INX
			_x++;
			_pc += 1;
			if (_x == 0) _sr |= SR_FLAG_Z;
			else _sr &= ~SR_FLAG_Z;
			if (_x & 0x80) _sr |= SR_FLAG_N;
			else _sr &= ~SR_FLAG_N;
			CPU_DEBUG("INX");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xE0:			//CPX			50
		case 0xE4:
		case 0xEC:
			switch (opcode & 0x0C) {
			case 0x00:		//immediate  (+2)
				core_cmp(_x, opcodes[1]);
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				core_cmp(_x, core_get_mem(opcodes[1]));
				_pc += 2;
				break;
			case 0x0C:		//absolute (+3)
				core_cmp(_x, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]));
				_pc += 3;
				break;
			}
			CPU_DEBUG("CPX");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xE1:			//SBC		
		case 0xE5:
		case 0xE9:
		case 0xF9:
		case 0xF5:
		case 0xF1:
		case 0xED:
		case 0xFD:
			switch (opcode & 0x1c) {
			case 0x00:		//(indirect, X)  (+2)
				address = (opcodes[1] + (uint16)_x) & 0xFF;
				_acc = core_sub(_acc, core_get_mem(USE_CARRY + core_get_word(address)));
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				_acc = core_sub(_acc, core_get_mem(opcodes[1]));
				_pc += 2;
				break;
			case 0x08:		//immdt  (+2)
				_acc = core_sub(_acc, opcodes[1]);
				_pc += 2;
				break;
			case 0x0c:		//absolute (+3)
				_acc = core_sub(_acc, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]));
				_pc += 3;
				break;
			case 0x10:		//(indirect), Y  (+2)
				_acc = core_sub(_acc, core_get_mem((USE_CARRY + core_get_word(opcodes[1])) + (uint16)_y));
				_pc += 2;
				break;
			case 0x14:		//zeropage, X  (+2)
				_acc = core_sub(_acc, core_get_mem((opcodes[1] + _x) & 0xFF));
				_pc += 2;
				break;
			case 0x18:		//absolute, Y (+3)
				_acc = core_sub(_acc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + _y ));
				_pc += 3;
				break;
			case 0x1c:		//absolute, X (+3)
				_acc = core_sub(_acc, core_get_mem((((uint16)opcodes[2] * 256) + opcodes[1]) + _x ));
				_pc += 3;
				break;
			}
			CPU_DEBUG("SBC");
			break;
		case 0xe6:			//INC
		case 0xeE:
		case 0xfE:
		case 0xf6:
			switch (opcode & 0x18) {
			case 0x00:		//zeropage  (+2)
				operand = core_get_mem(opcodes[1]);
				operand++;
				core_set_mem(opcodes[1], operand );
				_pc += 2;
				break;
			case 0x08:		//absolute (+3)
				operand = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
				operand++;
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand );
				_pc += 3;
				break;
			case 0x10:		//zeropage, X  (+2)
				address = (opcodes[1] + _x) & 0xFF;
				operand = core_get_mem(address);
				operand++;
				core_set_mem(address, operand );
				_pc += 2;
				break;
			case 0x18:		//absolute, X (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _x;
				operand = core_get_mem( address );
				operand++;
				core_set_mem( address , operand );
				_pc += 3;
				break;
			}
			if (operand == 0) _sr |= SR_FLAG_Z;
			else _sr &= ~SR_FLAG_Z;
			if (operand & 0x80) _sr |= SR_FLAG_N;
			else _sr &= ~SR_FLAG_N;
			CPU_DEBUG("INC");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xea:			//NOP
			_pc += 1;
			CPU_DEBUG("NOP");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xF0:			//BEQ  branch on equal
			if ((_sr & SR_FLAG_Z)) _pc += (int8)opcodes[1];
			_pc += 2;
			CPU_DEBUG("BEQ");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xD0:			//BNE  branch on not equal
			if ((_sr & SR_FLAG_Z) == 0) _pc += (int8)opcodes[1];
			_pc += 2;
			CPU_DEBUG("BNE");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xB0:			//BCS	branch on carry set	40
			if ((_sr & SR_FLAG_C)) _pc += (int8)opcodes[1];
			_pc += 2;
			CPU_DEBUG("BCS");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x90:			//BCC  branch on carry clear
			if ((_sr & SR_FLAG_C) == 0) _pc += (int8)opcodes[1];
			_pc += 2;
			CPU_DEBUG("BCC");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x70:			//BVS   branch on overflow set
			if ((_sr & SR_FLAG_V)) _pc += (int8)opcodes[1];
			_pc += 2;
			CPU_DEBUG("BVS");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x50:			//BVC  branch on overflow clear
			if ((_sr & SR_FLAG_V) == 0) _pc += (int8)opcodes[1];
			_pc += 2;
			CPU_DEBUG("BVC");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x30:			//BMI   branch on minus
			if ((_sr & SR_FLAG_N)) _pc += (int8)opcodes[1];
			_pc += 2;
			CPU_DEBUG("BMI");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x10:			//BPL branch on plus
			if ((_sr & SR_FLAG_N) == 0) _pc += (int8)opcodes[1];
			_pc += 2;
			CPU_DEBUG("BPL");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xF8:			//SED
			_sr |= SR_FLAG_D;
			_pc += 1;
			CPU_DEBUG("SED");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x78:			//SEI
			_sr |= SR_FLAG_I;
			_pc += 1;
			CPU_DEBUG("SEI");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x38:			//SEC
			_sr |= SR_FLAG_C;
			_pc += 1;
			CPU_DEBUG("SEC");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;

		//ILLEGAL INSTRUCTION CODES
		case 0x1A:			//illegal nop
		case 0x3A:	
		case 0x5A:	
		case 0x7A:
		case 0xDA:	
		case 0xFA:
			_pc += 1;
			CPU_DEBUG("NOP");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
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
			_pc += 2;
			CPU_DEBUG("NOP");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0x0c:			//illegal nop
		case 0x1c:
		case 0x3c:
		case 0x5c:
		case 0x7c:
		case 0xdc:
		case 0xfc:
			_pc += 3;
			CPU_DEBUG("NOP");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xAB:			//LAX #immdt
			_acc = core_lda(_acc, opcodes[1]);
			_x = _acc;
			_pc += 2;
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
				address = (opcodes[1] + (uint16)_x) & 0xFF;
				_acc = core_lda(_acc, core_get_mem(USE_CARRY + core_get_word(address)));
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				_acc = core_lda(_acc, core_get_mem(opcodes[1]));
				_pc += 2;
				break;
			case 0x0c:		//absolute (+3)
				_acc = core_lda(_acc, core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]));
				_pc += 3;
				break;
			case 0x10:		//(indirect), Y  (+2)
				_acc = core_lda(_acc, core_get_mem((USE_CARRY + core_get_word(opcodes[1])) + (uint16)_y));
				_pc += 2;
				break;
			case 0x14:		//zeropage, Y  (+2)
				_acc = core_lda(_acc, core_get_mem((opcodes[1] + _y) & 0xff));
				_pc += 2;
				break;
			case 0x1c:		//absolute, Y (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _y; 
				_acc = core_lda(_acc, core_get_mem(address));
				_pc += 3;
				break;
			}
			_x = _acc;
			CPU_DEBUG("LAX");
			break;
		case 0x83:			//SAX
		case 0x87:
		case 0x8F:
		case 0x97:
			operand = _acc & _x;
			switch (opcode & 0x1c) {
			case 0x00:		//(indirect, X)  (+2)
				address = (opcodes[1] + (uint16)_x) & 0xFF;
				core_set_mem(USE_CARRY + core_get_word(address), operand);
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				core_set_mem(opcodes[1], operand);
				_pc += 2;
				break;
			case 0x0c:		//absolute (+3)
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
				_pc += 3;
				break;
			case 0x14:		//zeropage, Y  (+2)
				core_set_mem((opcodes[1] + _y) & 0xFF, operand);
				_pc += 2;
				break;
			}
			CPU_DEBUG("SAX");
			goto skip_flag_test;			//skip checking for accumulator value (zero flag, negative flag)
			break;
		case 0xDB:			//DCP
			address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _y; 
			operand = core_get_mem(address);
			core_set_mem(address, operand - 1);
			_pc += 3;
			core_cmp(_acc, operand - 1);

			CPU_DEBUG("DCP");
			goto skip_flag_test;
			break;
		case 0xC7:			//DCP
		case 0xD7:
		case 0xCF:
		case 0xDF:
		case 0xC3:
		case 0xD3:
			switch (opcode & 0x1C) {
			case 0x00:		//(indirect, X)  (+2)
				address = (opcodes[1] + (uint16)_x) & 0xFF;
				operand = core_get_mem( USE_CARRY + core_get_word(address) );
				core_set_mem( USE_CARRY + core_get_word(address), operand - 1);
				_pc += 2;
				break;
			case 0x10:		//(indirect), Y  (+2)
				address = USE_CARRY + (core_get_word(opcodes[1])) + (uint16)_y;
				operand = core_get_mem(address);
				core_set_mem(address, operand - 1);
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				operand = core_get_mem(opcodes[1]);
				core_set_mem(opcodes[1], operand - 1);
				_pc += 2;
				break;
			case 0x0c:		//absolute (+3)
				operand = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand - 1);
				_pc += 3;
				break;
			case 0x14:		//zeropage, X  (+2)
				address = (opcodes[1] + _x) & 0xFF;
				operand = core_get_mem(address);
				core_set_mem(address, operand - 1);
				_pc += 2;
				break;
			case 0x1c:		//absolute, X (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _x; 
				operand = core_get_mem(address);
				core_set_mem(address, operand - 1);
				_pc += 3;
				break;
			}
			core_cmp(_acc, operand - 1);
			CPU_DEBUG("DCP");
			goto skip_flag_test;
			break;
		case 0x3B:				//RLA  absolute, Y (+3)
			address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _y; 
			operand = core_rol(core_get_mem(address), 1);
			core_set_mem(address, operand);
			_pc += 3;
			_acc = core_and(_acc, operand);
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
				address = (opcodes[1] + (uint16)_x) & 0xFF;
				operand = core_rol(core_get_mem( USE_CARRY + core_get_word(address) ), 1);
				core_set_mem( USE_CARRY + core_get_word(address) , operand);
				_pc += 2;
				break;
			case 0x10:		//(indirect), Y  (+2)
				address = USE_CARRY + (core_get_word(opcodes[1])) + (uint16)_y;
				operand = core_rol(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				operand = core_rol(core_get_mem(opcodes[1]), 1);
				core_set_mem(opcodes[1], operand);
				_pc += 2;
				break;
			case 0x0c:		//absolute (+3)
				operand = core_rol(core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), 1);
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
				_pc += 3;
				break;
			case 0x14:		//zeropage, X  (+2)
				address = (opcodes[1] + _x) & 0xFF;
				operand = core_rol(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 2;
				break;
			case 0x1c:		//absolute, X (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _x;
				operand = core_rol(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 3;
				break;
			}
			_acc = core_and(_acc, operand);
			CPU_DEBUG("RLA");
			break;
		case 0x7B:				//RRA  absolute, Y (+3)
			address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _y; 
			operand = core_ror(core_get_mem(address), 1);
			core_set_mem(address, operand);
			_pc += 3;
			_acc = core_add(_acc, operand);
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
				address = (opcodes[1] + (uint16)_x) & 0xFF;
				operand = core_ror(core_get_mem(USE_CARRY + core_get_word(address)), 1);
				core_set_mem( USE_CARRY + core_get_word(address) , operand);
				_pc += 2;
				break;
			case 0x10:		//(indirect), Y  (+2)
				address = USE_CARRY + (core_get_word(opcodes[1])) + (uint16)_y;
				operand = core_ror(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				operand = core_ror(core_get_mem(opcodes[1]), 1);
				core_set_mem(opcodes[1], operand);
				_pc += 2;
				break;
			case 0x0c:		//absolute (+3)
				operand = core_ror(core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), 1);
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
				_pc += 3;
				break;
			case 0x14:		//zeropage, X  (+2)
				address = (opcodes[1] + _x) & 0xFF;
				operand = core_ror(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 2;
				break;
			case 0x1c:		//absolute, X (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _x;
				operand = core_ror(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 3;
				break;
			}
			_acc = core_add(_acc, operand);
			CPU_DEBUG("RRA");
			break;
		case 0x5B:				//SRE  absolute, Y (+3)
			address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _y ;
			operand = core_lsr(core_get_mem(address), 1);
			core_set_mem(address, operand);
			_pc += 3;
			_acc = core_xor(_acc, operand);

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
				address = (opcodes[1] + (uint16)_x) & 0xFF;
				operand = core_lsr(core_get_mem( USE_CARRY + core_get_word(address) ), 1);
				core_set_mem( USE_CARRY + core_get_word(address) , operand);
				_pc += 2;
				break;
			case 0x10:		//(indirect), Y  (+2)
				address = USE_CARRY + (core_get_word(opcodes[1])) + (uint16)_y;
				operand = core_lsr(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				operand = core_lsr(core_get_mem(opcodes[1]), 1);
				core_set_mem(opcodes[1], operand);
				_pc += 2;
				break;
			case 0x0c:		//absolute (+3)
				operand = core_lsr(core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), 1);
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
				_pc += 3;
				break;
			case 0x14:		//zeropage, X  (+2)
				address = (opcodes[1] + _x) & 0xFF;
				operand = core_lsr(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 2;
				break;
			case 0x1c:		//absolute, X (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _x  ;
				operand = core_lsr(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 3;
				break;
			}
			_acc = core_xor(_acc, operand);
			CPU_DEBUG("SRE");
			break;
		case 0x9B:			//TAS
			address = ((uint16)opcodes[2] * 256) + opcodes[1];
			_sp = _acc & _x;
			operand = core_get_word(address) >> 8;
			core_set_mem(address, operand & _acc & _x);
			_pc += 3;
			CPU_DEBUG("TAS");
			break;
		case 0xEB:				//USBC  (SBC+NOP)
			_acc = core_sub(_acc, opcodes[1]);
			_pc += 2;
			CPU_DEBUG("USBC");
			break;
		case 0xFB:			//ISB
			address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _y; 
			operand = core_get_mem(address);
			core_set_mem(address, operand + 1);
			_pc += 3;
			_acc = core_sub(_acc, operand + 1);
			CPU_DEBUG("ISB");
			goto skip_flag_test;
			break;
		case 0xE7:			//ISB
		case 0xF7:
		case 0xEF:
		case 0xFF:
		case 0xE3:
		case 0xF3:
			switch (opcode & 0x1C) {
			case 0x00:		//(indirect, X)  (+2)
				address = (opcodes[1] + (uint16)_x) & 0xFF;
				operand = core_get_mem(USE_CARRY + core_get_word(address));
				core_set_mem(USE_CARRY + core_get_word(address), operand + 1);
				_pc += 2;
				break;
			case 0x10:		//(indirect), Y  (+2)
				address = USE_CARRY + (core_get_word(opcodes[1])) + (uint16)_y;
				operand = core_get_mem(address);
				core_set_mem(address, operand + 1);
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				operand = core_get_mem(opcodes[1]);
				core_set_mem(opcodes[1], operand + 1);
				_pc += 2;
				break;
			case 0x0c:		//absolute (+3)
				operand = core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]);
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand + 1);
				_pc += 3;
				break;
			case 0x14:		//zeropage, X  (+2)
				address = (opcodes[1] + _x) & 0xFF;
				operand = core_get_mem(address);
				core_set_mem(address, operand + 1);
				_pc += 2;
				break;
			case 0x1c:		//absolute, X (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _x ;
				operand = core_get_mem(address);
				core_set_mem(address, operand + 1);
				_pc += 3;
				break;
			}
			_acc = core_sub(_acc, operand + 1);
			CPU_DEBUG("ISB");
			goto skip_flag_test;
			break;
		case 0x1B:				//SLO  absolute, Y (+3)
			address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _y; 
			operand = core_asl(core_get_mem(address), 1);
			core_set_mem(address, operand);
			_pc += 3;
			_acc = core_orl(_acc, operand);
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
				address = (opcodes[1] + (uint16)_x) & 0xFF;
				operand = core_asl(core_get_mem(USE_CARRY + core_get_word(address)), 1);
				core_set_mem(USE_CARRY + core_get_word(address), operand);
				_pc += 2;
				break;
			case 0x10:		//(indirect), Y  (+2)
				address = USE_CARRY + (core_get_word(opcodes[1])) + (uint16)_y;
				operand = core_asl(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 2;
				break;
			case 0x04:		//zeropage  (+2)
				operand = core_asl(core_get_mem(opcodes[1]), 1);
				core_set_mem(opcodes[1], operand);
				_pc += 2;
				break;
			case 0x0c:		//absolute (+3)
				operand = core_asl(core_get_mem(((uint16)opcodes[2] * 256) + opcodes[1]), 1);
				core_set_mem(((uint16)opcodes[2] * 256) + opcodes[1], operand);
				_pc += 3;
				break;
			case 0x14:		//zeropage, X  (+2)
				address = (opcodes[1] + _x) & 0xFF;
				operand = core_asl(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 2;
				break;
			case 0x1c:		//absolute, X (+3)
				address = (((uint16)opcodes[2] * 256) + opcodes[1]) + _x; 
				operand = core_asl(core_get_mem(address), 1);
				core_set_mem(address, operand);
				_pc += 3;
				break;
			}
			_acc = core_orl(_acc, operand);
			CPU_DEBUG("SLO");
			break;
	}
	//check accumulator
	if (_acc == 0) _sr |= SR_FLAG_Z;
	else _sr &= ~SR_FLAG_Z;
	if (_acc & 0x80) _sr |= SR_FLAG_N;
	else _sr &= ~SR_FLAG_N;
skip_flag_test:
	_sr |= 0x20;							//ignore bit always one
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
	if ((ins_counter % 30001) == 0) {
		ppu_set_vblank(1);
	}
	if ((ins_counter % 16235) == 0) {
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