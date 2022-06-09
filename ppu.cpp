
/*
2000 | RW | PPU Control Register 1
| 0 - 1 | Name Table Address :
|     |
|     |           +---------- - +---------- - +
|     |           | 2 ($2800) | 3 ($2C00) |
|     |           +---------- - +---------- - +
|     |           | 0 ($2000) | 1 ($2400) |
|     |           +---------- - +---------- - +
|     |
|     | Remember that because of the mirroring there are only 2
|     | real Name Tables, not 4. Also, PPU will automatically
|     | switch to another Name Table when running off the current
|     | Name Table during scroll(see picture above).
| 2 | Vertical Write, 1 = PPU memory address increments by 32:
|     |
|     |    Name Table, VW = 0          Name Table, VW = 1
|     |   +---------------- + +---------------- +
|     |   |---- > write     |        | | write        |
|     |   |                |        | V              |
|     |
|   3 | Sprite Pattern Table Address, 1 = $1000, 0 = $0000.
| 4 | Screen Pattern Table Address, 1 = $1000, 0 = $0000.
| 5 | Sprite Size, 1 = 8x16, 0 = 8x8.
| 6 | PPU Master / Slave Mode, not used in NES.
| 7 | VBlank Enable, 1 = generate interrupts on VBlank.
------ + ---- - +-------------------------------------------------------------- -
$2001 | RW | PPU Control Register 2
| 0 | Unknown(? ? ? )
| 1 | Image Mask, 0 = don't show left 8 columns of the screen.
| 2 | Sprite Mask, 0 = don't show sprites in left 8 columns.
| 3 | Screen Enable, 1 = show picture, 0 = blank screen.
| 4 | Sprites Enable, 1 = show sprites, 0 = hide sprites.
| 5 - 7 | Background Color, 0 = black, 1 = blue, 2 = green, 4 = red.
|     | Do not use any other numbers as you may damage PPU hardware.
------ + ---- - +-------------------------------------------------------------- -
$2002 | R | PPU Status Register
| 0 - 5 | Unknown(? ? ? )
| 6 | Hit Flag, 1 = Sprite refresh has hit sprite #0.
|     | This flag resets to 0 when screen refresh starts
|     | (see "PPU Details").
| 7 | VBlank Flag, 1 = PPU is in VBlank state.
|     | This flag resets to 0 when VBlank ends or CPU reads $2002
|     | (see "PPU Details").
------ + ---- - +-------------------------------------------------------------- -
$2003 | W | Sprite Memory Address
|     | Used to set the address of the 256 - byte Sprite Memory to be
|     | accessed via $2004.This address will increment by 1 after
|     | each access to $2004.Sprite Memory contains coordinates,
|     | colors, and other sprite attributes(see "Sprites").
------ + ---- - +-------------------------------------------------------------- -
$2004 | RW | Sprite Memory Data
|     | Used to read / write the Sprite Memory.The address is set via
|     | $2003 and increments by 1 after each access.Sprite Memory
|     | contains coordinates, colors, and other sprite attributes
|     | sprites(see "Sprites").
------ + ---- - +-------------------------------------------------------------- -
$2005 | W | Screen Scroll Offsets
|     | There are two scroll registers, verticaland horizontal,
|     | which are both written via this port.The first value written
|     | will go into the Vertical Scroll Register(unless it is > 239,
    |     | then it will be ignored).The second value will appear in the
    |     | Horizontal Scroll Register.Name Tables are assumed to be
    |     | arranged in the following way :
|     |
|     |           +---------- - +---------- - +
|     |           | 2 ($2800) | 3 ($2C00) |
|     |           +---------- - +---------- - +
|     |           | 0 ($2000) | 1 ($2400) |
|     |           +---------- - +---------- - +
|     |
|     | When scrolled, the picture may span over several Name Tables.
|     | Remember that because of the mirroring there are only 2 real
|     | Name Tables, not 4.
------ + ---- - +-------------------------------------------------------------- -
$2006 | W | PPU Memory Address
|     | Used to set the address of PPU Memory to be accessed via
|     | $2007.The first write to this register will set 8 lower
|     | address bits.The second write will set 6 upper bits.The
|     | address will increment either by 1 or by 32 after each
|     | access to $2007(see "PPU Memory").
------ + ---- - +-------------------------------------------------------------- -
$2007 | RW | PPU Memory Data

*/

#include "stdafx.h"
#include "defs.h"

#define DISP_WIDTH          512
#define DISP_HEIGHT         480

static uint8 _pram[0x4000];
static uint8 _sprmem[0x100];
uint16 _cur_index = 0x2000;
uint16 _rd_index = 0x2000;
uint8 _spr_index = 0;
uint8 _cr1;
uint8 _cr2;
uint8 _psr;
uint8 _scroll_index = 0;
uint16 _vscroll = 0;
uint16 _hscroll = 0;
uint8 _ppu_config = 0;
uint8 _vblank = 0;

void ppu_set_vblank(uchar flag) {
    _vblank = flag;
}

uchar ppu_get_vblank() {
    uchar ret = 0;
    if (_cr1 & 0x80) {
        ret = _vblank;
        _vblank = 0;
    }
    return ret;
}

void ppu_dma_write(uint8* data, size_t size) {
    memcpy(_sprmem + _spr_index, data, size);
    _spr_index += size;
}

void ppu_set_ram(uint16 address, uint8* data, size_t size) {
    memcpy(_pram + address, data, size);
}


void ppu_set_cr1(uint8 data) { _cr1 = data; }
uchar ppu_get_cr1() { return _cr1; }
void ppu_set_cr2(uint8 data) { _cr2 = data; }
uchar ppu_get_cr2() { return _cr2; }
uchar hit_buffer[256 * 240];

void ppu_set_sr(uint8 data) { _psr = data; }

uchar ppu_get_sr() { 
    uchar psr ;
    _psr |= 0x80; 
    psr = _psr;
    if (_psr & 0x40) {
        _psr &= ~0x40;              //clear hit status
    } else {
        //check hit test
        for (uint16 i = 0; i < 256 * 240; i++) {
            if (hit_buffer[i] > 1) {
                _psr |= 0x40;       //set hit status
            }
        }
    }
    _scroll_index = 0;
    _cur_index = 0;
    return psr; 
}

void ppu_set_scroll(uint8 data) {
    if (_scroll_index == 0) {
        _hscroll = data;
        if (data != 0) {
            data = data;
        }
    }
    else if (_scroll_index == 1) {
        if (data <= 239) {    //skip if data > 239
            _vscroll = data;
        }
    }
    _scroll_index++;
}
uchar ppu_get_scroll() {
    if ((_scroll_index & 0x01) == 0) {
        return _vscroll;
    }
    else
        return _hscroll;
    return 0;
}
void ppu_set_spr_addr(uint8 data) { 
    _spr_index = data; 
}

uchar ppu_get_spr_addr() { 
    return _spr_index; 
}

void ppu_set_mem_addr(uint8 data) {
    _cur_index <<= 8;
    _cur_index |= data;
    _cur_index = _cur_index ;
    _rd_index = _cur_index;
}

uchar ppu_get_mem_addr() { return _cur_index; }

void ppu_set_spr_data(uint8 data) { 
    _sprmem[_spr_index++] = data; 
}

uchar ppu_get_spr_data() {
    uchar ret;
    ret = _sprmem[_spr_index];
    _spr_index++;
    return ret;
}

void ppu_set_mem_data(uint8 data) {
    if (_cur_index >= 0x4000) return;       //skip operation
    _pram[_cur_index] = data;
    if (_cr1 & 0x04) _cur_index += 32;      //vertical write
    else _cur_index++;
}

uchar ppu_get_mem_data() {
    if (_cur_index >= 0x4000) return 0;       //skip operation
    uchar ret = _pram[_cur_index];
    if (_cr1 & 0x04) _cur_index += 32;      //vertical write
    else _cur_index++;
    return ret;
}
const uint32 default_bkgcolor[] = { 0xFF000000, 0x000000FF, 0x0000FF00, 0x00, 0xFFFF0000, 0x00, 0x00, 0x00 };

uchar ppu_get_nametable(uint16 x, uint16 y) {
    x = x % 512;
    y = y % 480;
    if (x < 256) {
        if (y > 240) {
            return 2;
        }
        else {
            return 0;
        }
    }
    else {
        if (y > 240) {
            return 3;
        }
        else {
            return 1;
        }
    }
    return 0;
}

uint16 ppu_get_tablebase(uint8 table) {

    if ((_ppu_config & 0x08) == 0) {            //4 screen vram layout disabled
        if (_ppu_config & 0x01) {
            //vertical mirroring
            switch (table & 0x03) {
            case 0: return 0x2000;
            case 1: return 0x2400;
            case 2: return 0x2000;
            case 3: return 0x2400;
            }
        }
        else {
            //horizontal mirroring
            switch (table & 0x03) {
            case 0: return 0x2000;
            case 1: return 0x2000;
            case 2: return 0x2800;
            case 3: return 0x2800;
            }
        }
    }
    switch (table & 0x03) {
    case 0: return 0x2000;
    case 1: return 0x2400;
    case 2: return 0x2800;
    case 3: return 0x2c00;
    }
}

uchar ppu_get_tableattr(uint16 base, uint16 x, uint16 y) {
    uchar x_index = x / 32;
    uchar y_index = y / 32;
    uchar x_offset = (x % 32) >> 3;
    uchar y_offset = (y % 32) >> 3;
    uchar* attr_base = _pram + base + 0x3c0;
    //attr_base[8][8]
    uchar b_attr = attr_base[((y_index * 8) + x_index) ];
    switch ((x_offset << 4) | y_offset) {
    case 0x00:
    case 0x10:
    case 0x01:
    case 0x11: return b_attr & 0x03;
    case 0x20:
    case 0x30:
    case 0x21:
    case 0x31: return (b_attr >> 2) & 0x03;
    case 0x02:
    case 0x12:
    case 0x03:
    case 0x13: return (b_attr >> 4) & 0x03;
    case 0x22:
    case 0x32:
    case 0x23:
    case 0x33: return (b_attr >> 6) & 0x03;
    }
    return 0;
}

const uint32 _pal2col[] = { 
    0xFF797A78, 0xFF0031BB, 0xFF261ECB, 0xFF5B12BA, 0xFF87008B, 0xFF91001C, 0xFF820600, 0xFF5E2400, 0xFF2C3B00, 0xFF004900, 0xFF004E00, 0xFF004926, 0xFF004482, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFBFBFBD, 0xFF006EFF, 0xFF5457FF, 0xFF9047FF, 0xFFC939E6, 0xFFD92D77, 0xFFD14100, 0xFFAB5B00, 0xFF797100, 0xFF008300, 0xFF008B00, 0xFF008740, 0xFF0082B3, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFFFFFFF, 0xFF2DBEFF, 0xFF94A8FF, 0xFFCB99FF, 0xFFFF91FF, 0xFFFF8AE3, 0xFFFF945A, 0xFFFFA400, 0xFFD9B600, 0xFF85C900, 0xFF07D300, 0xFF00D474, 0xFF00D1DB, 0xFF646362, 0xFF000000, 0xFF000000,
    0xFFFFFFFF, 0xFFBCEBFF, 0xFFD8E3FF, 0xFFEDDDFF, 0xFFFFDAFF, 0xFFFFD8FC, 0xFFFFDCCB, 0xFFFFE1A4, 0xFFF5E78C, 0xFFD5EF8B, 0xFFBBF3A8, 0xFFAFF3CD, 0xFFAFF2F4, 0xFFC7C8C6, 0xFF646362, 0xFF000000,
};

uint32 pal2col(uchar pal) {
    return _pal2col[pal & 0x3f];
}

void ppu_init(uint8 config) {
    _ppu_config = config;
}


void ppu_render(uchar* vbuffer) {
    uint32* output = (uint32*)vbuffer;
    uint16 y_index, y_offset;
    uint16 x_index, x_offset;
    uint8 spr_width = 8;
    uint8 spr_size = (_cr1 & 0x20) ?32 : 16;
    uint8 spr_height = (_cr1 & 0x20) ? 16 : 8;
    uint32 bkg_color = default_bkgcolor[_cr2 >> 5];
    uint8 spr_en = (_cr2 & 0x10) ? 1 : 0;
    uint8 nm_table = 0;
    uint16 table_base;
    uint16 spr_index;
    uchar attr;
    uchar spr_offset;
    uchar pattern0;
    uchar pattern1;
    uchar pallete_index = 0;
    uchar pallete;
    uint16 p_index;
    int v_index;
    uchar x, y;
    uint16 screen_pattern_base = 0x0000;
    uint16 sprite_pattern_base = 0x0000;
    if (_cr1 & 0x10)screen_pattern_base = 0x1000;
    if (_cr1 & 0x08)sprite_pattern_base = 0x1000;           //default sprite pattern table
    if (_cr1 & 0x01) _hscroll |= 0x100;
    else _hscroll &= ~0x100;
    if (_cr1 & 0x02) _vscroll |= 0x100;
    else _vscroll &= ~0x100;
    //render background first
    uint32 bkgcolor = pal2col(_pram[0x3f00]);
    //_psr &= ~0x40;              //clear hit status
    memset(hit_buffer, 0, sizeof(hit_buffer));
    memset(output, 0, DISP_WIDTH * DISP_HEIGHT * sizeof(uint32));
    
    _pram[0x3f10] = _pram[0x3f00];
    _pram[0x3f0c] = _pram[0x3f08] = _pram[0x3f04] = _pram[0x3f00];
    _pram[0x3f1c] = _pram[0x3f18] = _pram[0x3f14] = _pram[0x3f10];

    //render oam background
    for (uint16 k = 0; k < 256; k += 4) {
        y = _sprmem[k];                 //y location
        p_index = _sprmem[k + 1];     //tile index
        x = _sprmem[k + 3];               //x location
        attr = _sprmem[k + 2];     //tile index
        if ((attr & 0x20) == 0) continue;                //render only foreground
        if (spr_size == 32) {       //8x16 pixel
            sprite_pattern_base = (_sprmem[k + 1] & 0x01) ? 0x1000 : 0x0000;
        }
        //p_index >>= 1;

        for (uint16 j = 0; j < spr_height; j++) {     //height 1 tile (8) or 2 tile (16)
            if (attr & 0x80) y_offset = (spr_height - (j + 1));      //flip vertical
            else y_offset = j;              //normal vertical
            y_index = (j / spr_height) % 30;
            for (uint16 i = 0; i < 8; i++) {        //width always 8
                if (attr & 0x40) spr_offset = (i % 8);      //flip horizontal
                else spr_offset = 7 - (i % 8);              //normal horizontal
                //p_index = (spr_index * spr_size) + y_offset;          //pattern index
                pattern0 = _pram[sprite_pattern_base + (p_index * 16) + y_offset];
                pattern1 = _pram[sprite_pattern_base + (p_index * 16) + y_offset + 8];      //[p0:8][p1:8]
                pallete_index = 0;
                if (pattern0 & (1 << spr_offset)) pallete_index |= 0x01;        //bit 0
                if (pattern1 & (1 << spr_offset)) pallete_index |= 0x02;        //bit 1

                if (pallete_index == 0) continue;
                pallete = _pram[0x3f10 + ((attr & 0x03) << 2) + pallete_index];           //locate color pallete on image pallete (0x3f10)
                if (pallete == 0) continue;
                if (pallete != 0) {
                    hit_buffer[(j * 256) + i] = hit_buffer[(j * 256) + i] + 1;
                }
                v_index = (((y + j) * DISP_WIDTH) + (i + x));
                v_index *= 2;
                //v_index += (i - _hscroll) * 2;
                output[v_index] = pal2col(pallete);
                output[v_index + 1] = pal2col(pallete);
                output[v_index + DISP_WIDTH] = pal2col(pallete);
                output[v_index + DISP_WIDTH + 1] = pal2col(pallete);

            }
        }
    }
    uint8 prev_nmtable;
    //start rendering nametables (background)
    for (uint16 j = _vscroll; j < (_vscroll + 240); j++) {
        y = ((j - _vscroll) % 240);
        y_offset = (j % 8);
        y_index = (j / 8) % 30;
        if (_vscroll != 0) {
            y = y;
        }
        prev_nmtable = 0;
        for (uint16 i = _hscroll; i < (_hscroll + 256); i++) {
            x = ((i - _hscroll) % 256);
            x_offset = i % spr_width;
            x_index = (i / spr_width) % 32;
            nm_table = ppu_get_nametable(i % 512, j % 480);
            table_base = ppu_get_tablebase(nm_table);
            if (nm_table == 1) {
                y = y;
                prev_nmtable = nm_table;
            }
            spr_index = _pram[ table_base + (y_index * 32) + x_index];
            spr_offset = 7 - (i % 8);
            //spr_index = 0;
            ///attr = ppu_get_tableattr(table_base, x_index, y_index);     //index of nametable
            attr = ppu_get_tableattr(table_base, i % 256, j %240);
            //attr = 1;
            
            p_index = (spr_index * 16) + y_offset;          //pattern index
            pattern0 = _pram[screen_pattern_base + p_index];
            pattern1 = _pram[screen_pattern_base + p_index + 8] ;      //[p0:8][p1:8]
            pallete_index = 0;
            if (pattern0 & (1 << spr_offset)) pallete_index |= 0x01;        //bit 0
            if (pattern1 & (1 << spr_offset)) pallete_index |= 0x02;        //bit 1
            
            pallete = _pram[0x3f00 + (attr << 2) + pallete_index ];           //locate color pallete on image pallete (0x3f10)
            if (pallete == 0) continue;
            //upscale 2x
            if (pallete != 0) {
                hit_buffer[(y * 256) + x] = hit_buffer[(y * 256) + x] + 1;
            }
            v_index = (y * DISP_WIDTH) + x;
            v_index *= 2;
            //v_index += (i - _hscroll) * 2;
            output[v_index] = pal2col(pallete);
            output[v_index + 1] = pal2col(pallete);
            output[v_index + DISP_WIDTH] = pal2col(pallete);
            output[v_index + DISP_WIDTH + 1] = pal2col(pallete);
            
            //printf("%02X ", spr_offset);
        }
        //printf("\r\n");
    }
    
    //render oam foreground
    for (uint16 k = 0; k < 256; k += 4) {
        y = _sprmem[k];                 //y location
        p_index = _sprmem[k + 1];     //tile index
        x = _sprmem[k + 3];               //x location
        attr = _sprmem[k + 2];     //tile index
        if ((attr & 0x20)) continue;                //render only foreground
        if (spr_size == 32) {       //8x16 pixel
            sprite_pattern_base = (_sprmem[k + 1] & 0x01) ? 0x1000 : 0x0000;
        }
        //p_index >>= 1;

        for (uint16 j = 0; j < spr_height; j++) {     //height 1 tile (8) or 2 tile (16)
            if (attr & 0x80) y_offset = (spr_height - (j + 1));      //flip vertical
            else y_offset = j;              //normal vertical
            y_index = (j  / spr_height) % 30;
            for (uint16 i = 0; i < 8; i++) {        //width always 8
                if(attr & 0x40) spr_offset = (i % 8);           //flip horizontal
                else spr_offset = 7 - (i % 8);              //normal horizontal
                //p_index = (spr_index * spr_size) + y_offset;          //pattern index
                pattern0 = _pram[sprite_pattern_base + (p_index*16) + y_offset];
                pattern1 = _pram[sprite_pattern_base + (p_index*16) + y_offset + 8];      //[p0:8][p1:8]
                pallete_index = 0;
                if (pattern0 & (1 << spr_offset)) pallete_index |= 0x01;        //bit 0
                if (pattern1 & (1 << spr_offset)) pallete_index |= 0x02;        //bit 1

                if (pallete_index == 0) continue;
                pallete = _pram[0x3f10 + ((attr & 0x03) << 2) + pallete_index];           //locate color pallete on image pallete (0x3f10)
                if (pallete == 0) continue;
                if (pallete != 0) {
                    hit_buffer[(j * 256) + i] = hit_buffer[(j * 256) + i] + 1;
                }
                if (pallete_index > 0) {
                    y = y;
                }
                v_index = (((y + j)  * DISP_WIDTH) + (i + x));
                v_index *= 2;
                //v_index += (i - _hscroll) * 2;
                output[v_index] = pal2col(pallete);
                output[v_index + 1] = pal2col(pallete);
                output[v_index + DISP_WIDTH] = pal2col(pallete);
                output[v_index + DISP_WIDTH + 1] = pal2col(pallete);

            }
        }
    }

    //hit test
    for (uint16 i = 0; i < 256 * 240; i++) {
        if (hit_buffer[i] > 1) {
            _psr |= 0x40;       //set hit status
        }
    }


    /*
    printf("attrs\r\n");
    table_base = 0x2000;
    for (uint16 i = table_base + 0x3c0; i < table_base + 0x400; i++) {
        printf("%02X ", _pram[i]);
    }
    printf("=====\r\n");

    printf("palletes\r\n");
    printf("%02X %02X %02X %02X\r\n", _pram[0x3f00], _pram[0x3f01], _pram[0x3f02], _pram[0x3f03]);
    printf("%02X %02X %02X %02X\r\n", _pram[0x3f04], _pram[0x3f05], _pram[0x3f06], _pram[0x3f07]);
    printf("%02X %02X %02X %02X\r\n", _pram[0x3f08], _pram[0x3f09], _pram[0x3f0A], _pram[0x3f0B]);
    printf("%02X %02X %02X %02X\r\n", _pram[0x3f0C], _pram[0x3f0D], _pram[0x3f0E], _pram[0x3f0F]);
    printf("=====\r\n");
    */
    //start rendering sprites
}