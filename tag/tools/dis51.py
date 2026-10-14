#!/usr/bin/env python3
# Minimal 8051 disassembler. usage: dis51.py file start end
import sys

SFR = {0x80:'P0',0x81:'SP',0x82:'DPL',0x83:'DPH',0x87:'PCON',0x88:'TCON',0x89:'TMOD',0x90:'P1',0x98:'SCON',0x99:'SBUF',
       0xA0:'P2',0xA8:'IE',0xB0:'P3',0xB8:'IP',0xD0:'PSW',0xE0:'ACC',0xF0:'B'}

def bit(b):
    if b < 0x80: return f"{0x20+b//8:02X}h.{b%8}"
    base = b & 0xF8
    return f"{SFR.get(base, '%02Xh'%base)}.{b&7}"

def dir_(d): return SFR.get(d, f"{d:02X}h")

def dis(code, pc):
    op = code[pc]
    b1 = code[pc+1] if pc+1 < len(code) else 0
    b2 = code[pc+2] if pc+2 < len(code) else 0
    rel = lambda off, n: (pc + n + (off - 256 if off > 127 else off)) & 0xFFFF
    lo = op & 0x0F; hi = op >> 4
    # aJMP/aCALL
    if lo == 1:
        a = ((pc + 2) & 0xF800) | ((op >> 5) << 8) | b1
        return 2, ("ACALL" if op & 0x10 else "AJMP") + f" {a:04X}"
    regs = ['@R0','@R1'] + [f'R{i}' for i in range(8)]
    def src(l):  # l in 6..F
        return regs[l-6]
    T = {
     0x00:(1,'NOP'),0x02:(3,f'LJMP {b1<<8|b2:04X}'),0x03:(1,'RR A'),0x04:(1,'INC A'),0x05:(2,f'INC {dir_(b1)}'),
     0x10:(3,f'JBC {bit(b1)},{rel(b2,3):04X}'),0x12:(3,f'LCALL {b1<<8|b2:04X}'),0x13:(1,'RRC A'),0x14:(1,'DEC A'),0x15:(2,f'DEC {dir_(b1)}'),
     0x20:(3,f'JB {bit(b1)},{rel(b2,3):04X}'),0x22:(1,'RET'),0x23:(1,'RL A'),0x24:(2,f'ADD A,#{b1:02X}'),0x25:(2,f'ADD A,{dir_(b1)}'),
     0x30:(3,f'JNB {bit(b1)},{rel(b2,3):04X}'),0x32:(1,'RETI'),0x33:(1,'RLC A'),0x34:(2,f'ADDC A,#{b1:02X}'),0x35:(2,f'ADDC A,{dir_(b1)}'),
     0x40:(2,f'JC {rel(b1,2):04X}'),0x42:(2,f'ORL {dir_(b1)},A'),0x43:(3,f'ORL {dir_(b1)},#{b2:02X}'),0x44:(2,f'ORL A,#{b1:02X}'),0x45:(2,f'ORL A,{dir_(b1)}'),
     0x50:(2,f'JNC {rel(b1,2):04X}'),0x52:(2,f'ANL {dir_(b1)},A'),0x53:(3,f'ANL {dir_(b1)},#{b2:02X}'),0x54:(2,f'ANL A,#{b1:02X}'),0x55:(2,f'ANL A,{dir_(b1)}'),
     0x60:(2,f'JZ {rel(b1,2):04X}'),0x62:(2,f'XRL {dir_(b1)},A'),0x63:(3,f'XRL {dir_(b1)},#{b2:02X}'),0x64:(2,f'XRL A,#{b1:02X}'),0x65:(2,f'XRL A,{dir_(b1)}'),
     0x70:(2,f'JNZ {rel(b1,2):04X}'),0x72:(2,f'ORL C,{bit(b1)}'),0x73:(1,'JMP @A+DPTR'),0x74:(2,f'MOV A,#{b1:02X}'),0x75:(3,f'MOV {dir_(b1)},#{b2:02X}'),
     0x80:(2,f'SJMP {rel(b1,2):04X}'),0x82:(2,f'ANL C,{bit(b1)}'),0x83:(1,'MOVC A,@A+PC'),0x84:(1,'DIV AB'),0x85:(3,f'MOV {dir_(b2)},{dir_(b1)}'),
     0x90:(3,f'MOV DPTR,#{b1<<8|b2:04X}'),0x92:(2,f'MOV {bit(b1)},C'),0x93:(1,'MOVC A,@A+DPTR'),0x94:(2,f'SUBB A,#{b1:02X}'),0x95:(2,f'SUBB A,{dir_(b1)}'),
     0xA0:(2,f'ORL C,/{bit(b1)}'),0xA2:(2,f'MOV C,{bit(b1)}'),0xA3:(1,'INC DPTR'),0xA4:(1,'MUL AB'),0xA5:(1,'???'),
     0xB0:(2,f'ANL C,/{bit(b1)}'),0xB2:(2,f'CPL {bit(b1)}'),0xB3:(1,'CPL C'),0xB4:(3,f'CJNE A,#{b1:02X},{rel(b2,3):04X}'),0xB5:(3,f'CJNE A,{dir_(b1)},{rel(b2,3):04X}'),
     0xC0:(2,f'PUSH {dir_(b1)}'),0xC2:(2,f'CLR {bit(b1)}'),0xC3:(1,'CLR C'),0xC4:(1,'SWAP A'),0xC5:(2,f'XCH A,{dir_(b1)}'),
     0xD0:(2,f'POP {dir_(b1)}'),0xD2:(2,f'SETB {bit(b1)}'),0xD3:(1,'SETB C'),0xD4:(1,'DA A'),0xD5:(3,f'DJNZ {dir_(b1)},{rel(b2,3):04X}'),
     0xE0:(1,'MOVX A,@DPTR'),0xE2:(1,'MOVX A,@R0'),0xE3:(1,'MOVX A,@R1'),0xE4:(1,'CLR A'),0xE5:(2,f'MOV A,{dir_(b1)}'),
     0xF0:(1,'MOVX @DPTR,A'),0xF2:(1,'MOVX @R0,A'),0xF3:(1,'MOVX @R1,A'),0xF4:(1,'CPL A'),0xF5:(2,f'MOV {dir_(b1)},A'),
    }
    if op in T: return T[op]
    if lo >= 6:
        s = src(lo)
        m = {0x0:f'INC {s}',0x1:f'DEC {s}',0x2:f'ADD A,{s}',0x3:f'ADDC A,{s}',0x4:f'ORL A,{s}',0x5:f'ANL A,{s}',0x6:f'XRL A,{s}',
             0x8:f'MOV {dir_(b1)},{s}',0x9:f'SUBB A,{s}',0xC:f'XCH A,{s}',0xE:f'MOV A,{s}',0xF:f'MOV {s},A'}
        if hi in m: return (2 if hi==8 else 1), m[hi]
        if hi == 0x7: return 2, f'MOV {s},#{b1:02X}'
        if hi == 0xA: return 2, f'MOV {s},{dir_(b1)}'
        if hi == 0xB: return 3, f'CJNE {s},#{b1:02X},{rel(b2,3):04X}'
        if hi == 0xD:
            if lo in (6,7): return 1, f'XCHD A,{s}'
            return 2, f'DJNZ {s},{rel(b1,2):04X}'
    return 1, f'db {op:02X}'

if __name__ == '__main__':
    code = open(sys.argv[1], 'rb').read()
    pc = int(sys.argv[2], 16); end = int(sys.argv[3], 16)
    while pc < end:
        n, t = dis(code, pc)
        print(f"{pc:04X}: {code[pc:pc+n].hex(' '):9} {t}")
        pc += n
