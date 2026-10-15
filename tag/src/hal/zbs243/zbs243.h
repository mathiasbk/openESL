/*
 * zbs243.h - the ZBS243 (SEM9110) special function registers this HAL uses.
 *
 * Only the registers in use are listed. The addresses and bit meanings come from
 * Dmitry Grinberg's reverse engineering of the ZBS243 (dmitry.gr), as published in
 * his zbs243.h and carried on in atc1441's ZBS_Flasher examples and the
 * OpenEPaperLink tag firmware (Tag_FW_ZBS243). Each one used is listed in
 * docs/kildelogg.md, with what was confirmed on our own tags.
 *
 * Some SFR addresses mean different registers depending on CFGPAGE (bit 0x04).
 * The comment says which page a register needs; page 0 is the default.
 */
#ifndef ZBS243_H
#define ZBS243_H

/* ---- Core, clock, config page ---- */
__sfr __at (0x8E) CLKSPEED;  /* 0x01 = 16 MHz crystal, CPU undivided. 0x21 needed for flash operations */
__sfr __at (0xB7) CLKEN;     /* peripheral clocks: 0x01 timers, 0x08 SPI, 0x20 UART */
__sfr __at (0xFF) CFGPAGE;   /* 0x04 selects config page 4 */
__sfr __at (0xA8) IEN0;      /* interrupt enable, bit 7 = EA */
__sfr __at (0x8F) RESET;     /* bit 0x04 holds the radio in reset (active high) */
__sfr __at (0xA1) IEN1;      /* bit 0x10: temperature conversion (cleared when done) */

/* ---- GPIO (page 0) ---- */
__sfr __at (0x80) P0;        /* reading an output pin does NOT return the written level */
__sfr __at (0x90) P1;
__sfr __at (0xA0) P2;        /* also XPAGE for movx @Ri; we do not use __pdata */
__sfr __at (0xAD) P0FUNC;    /* 1 = peripheral function, 0 = GPIO */
__sfr __at (0xAE) P1FUNC;
__sfr __at (0xAF) P2FUNC;
__sfr __at (0xB9) P0DIR;     /* 1 = input, 0 = output */
__sfr __at (0xBA) P1DIR;
__sfr __at (0xBB) P2DIR;
__sfr __at (0xBC) P0PULL;    /* 1 = pull-up */
__sfr __at (0xBD) P1PULL;
__sfr __at (0xBE) P2PULL;
__sfr __at (0xA6) P0INTEN;
__sfr __at (0xA7) P1INTEN;
__sfr __at (0xA9) P2INTEN;

__sbit __at (0x80) P0_0;
__sbit __at (0x81) P0_1;
__sbit __at (0x82) P0_2;
__sbit __at (0x90) P1_0;
__sbit __at (0x92) P1_2;
__sbit __at (0x97) P1_7;
__sbit __at (0xA0) P2_0;
__sbit __at (0xA1) P2_1;
__sbit __at (0xA2) P2_2;


/* ---- Timer 0 (classic 8051, clocked at 16 MHz / 12) ---- */
__sfr __at (0x88) TCON;      /* 0x10 TR0 (run), 0x20 TF0 (overflow) */
__sfr __at (0x89) TMOD;
__sfr __at (0x8A) TL0;
__sfr __at (0x8C) TH0;

/* ---- UART (page 0). TX is on P0.6 (debug pad 4) when P0FUNC bit 6 is set ---- */
__sfr __at (0x98) UARTSTA;   /* 0x10 enable, 0x02 TX ready, 0x01 RX byte. Never test with jbc */
__sfr __at (0x99) UARTBUF;
__sfr __at (0x9A) UARTBRGL;  /* baud = 16 MHz / (BRG + 1) */
__sfr __at (0x9B) UARTBRGH;

/* ---- Watchdog (page 4; the same addresses are P1DIR/P2DIR on page 0) ---- */
__sfr __at (0xBA) WDTENA;    /* bit 0 = on */
__sfr __at (0xBB) WDTPET;    /* write 0 to restart the count */
__sfr __at (0xBC) WDTRSTVALL;
__sfr __at (0xBD) WDTRSTVALM;
__sfr __at (0xBE) WDTRSTVALH;
__sfr __at (0xBF) WDTCONF;   /* bit 7 = enable */

/* ---- Temperature sensor (page 4; CLKEN bit 0x80). Result bit-reversed ---- */
__sfr __at (0xE6) TEMPCAL1;
__sfr __at (0xE7) TEMPCAL2;
__sfr __at (0xF7) TEMPCFG;   /* 0x08 set when a conversion is done */
__sfr __at (0xF8) TEMPRETH;  /* top 8 bits of the 10-bit result, bit-reversed */
__sfr __at (0xF9) TEMPRETL;  /* low 2 bits in bits 0-1, swapped */
__sfr __at (0xFB) TEMPCAL3;
__sfr __at (0xFC) TEMPCAL4;
__sfr __at (0xFD) TEMPCAL5;
__sfr __at (0xFE) TEMPCAL6;

/* ---- Flash DMA (page 4). Copies flash or the info page to xdata ---- */
__sfr __at (0xC7) SETTINGS;  /* 0x08 read, 0x18 write, |= 0x38 erase page then write */
__sfr __at (0xCF) TCON2;     /* 0x08 = flash operation done */
__sfr __at (0xD7) TRIGGER;   /* 0x08 starts a flash operation */
__sfr __at (0xD8) FPGNO;     /* page 0x00-0x3F main flash (1 KB each), 0x80 info page */
__sfr __at (0xD9) FWRSRCL;   /* read: offset in page */
__sfr __at (0xDA) FWRSRCH;
__sfr __at (0xDB) FWRDSTL;   /* read: xdata destination */
__sfr __at (0xDC) FWRDSTH;
__sfr __at (0xDD) FWRLENL;   /* length - 1 */
__sfr __at (0xDE) FWRLENH;
__sfr __at (0xDF) FWRTHREE;  /* always 3 */

/* ---- Radio, SFRs (page 4 unless noted) ---- */
__sfr __at (0xC1) RADIO_UNK_C1;
__sfr __at (0xC9) RADIO_TXPTRL;   /* xdata address of the TX buffer's length byte */
__sfr __at (0xCA) RADIO_TXPTRH;
__sfr __at (0xCB) RADIO_INITSEQ1;
__sfr __at (0xCC) RADIO_INITSEQ2;
__sfr __at (0xCD) RADIO_TXLEN;    /* frame bytes to send, without the length byte and FCS */
__sfr __at (0xCE) RADIO_INITSEQ0;
__sfr __at (0xD1) RADIO_INITSEQ4;
__sfr __at (0xD2) RADIO_INITSEQ5;
__sfr __at (0xD3) RADIO_RXPTRL;   /* xdata destination of the RX DMA */
__sfr __at (0xD4) RADIO_RXPTRH;
__sfr __at (0xD5) RADIO_RXLEN;
__sfr __at (0xD6) RADIO_INITSEQ3;
__sfr __at (0xFA) RADIO_GOTLEN;   /* any page: length of the frame in the RX FIFO */
/* TRIGGER bits: 0x04 start RX DMA, 0x02 start TX DMA. TCON2 bits: 0x04 RX DMA done,
 * 0x02 TX DMA done, 0x08 radio init (or flash operation) done. */

/* ---- Radio, memory mapped in xdata ---- */
#define RADIO_REG(a)    (*(volatile __xdata uint8_t *)(a))
#define RADIO_COMMAND       RADIO_REG(0xDF48)
#define RADIO_CAL_70        RADIO_REG(0xDF70)
#define RADIO_CAL_71        RADIO_REG(0xDF71)
#define RADIO_FLAGS         RADIO_REG(0xDF80)
#define RADIO_CAL_81        RADIO_REG(0xDF81)
#define RADIO_UNK_83        RADIO_REG(0xDF83)
#define RADIO_CAL_86        RADIO_REG(0xDF86)
#define RADIO_OWN_MAC       ((volatile __xdata uint8_t *)0xDF88)  /* 8 bytes, most significant first */
#define RADIO_PAN_HI        RADIO_REG(0xDF90)
#define RADIO_PAN_LO        RADIO_REG(0xDF91)
#define RADIO_SHORT_HI      RADIO_REG(0xDF92)
#define RADIO_SHORT_LO      RADIO_REG(0xDF93)
#define RADIO_CAL_94        RADIO_REG(0xDF94)
#define RADIO_CAL_95        RADIO_REG(0xDF95)
#define RADIO_CAL_96        RADIO_REG(0xDF96)
#define RADIO_RX_FIRST      RADIO_REG(0xDF98)   /* first byte of the frame in the RX FIFO */
#define RADIO_RF_STATE      RADIO_REG(0xDF9B)   /* 0x80 = transmitting */
#define RADIO_CAL_9D        RADIO_REG(0xDF9D)
#define RADIO_CAL_A1        RADIO_REG(0xDFA1)
#define RADIO_UNK_A6        RADIO_REG(0xDFA6)
#define RADIO_IRQ_PENDING   RADIO_REG(0xDFAD)   /* 0x40 TX/ACK window over, 0x20 frame in RX FIFO, 0x10 ACK seen */
#define RADIO_IRQ_ENABLE    RADIO_REG(0xDFAF)
#define RADIO_CHANNEL       RADIO_REG(0xDFC0)
#define RADIO_UNK_C1X       RADIO_REG(0xDFC1)
#define RADIO_CAL_C2        RADIO_REG(0xDFC2)
#define RADIO_CAL_C3        RADIO_REG(0xDFC3)
#define RADIO_CAL_C4        RADIO_REG(0xDFC4)
#define RADIO_CAL_C5        RADIO_REG(0xDFC5)
#define RADIO_CAL_C6        RADIO_REG(0xDFC6)
#define RADIO_CAL_C7        RADIO_REG(0xDFC7)
#define RADIO_UNK_C8        RADIO_REG(0xDFC8)
#define RADIO_TX_POWER      RADIO_REG(0xDFC9)   /* 0..7 */
#define RADIO_UNK_CA        RADIO_REG(0xDFCA)
#define RADIO_CH_SETTING2   RADIO_REG(0xDFCB)
#define RADIO_UNK_CD        RADIO_REG(0xDFCD)
#define RADIO_UNK_CE        RADIO_REG(0xDFCE)
#define RADIO_CAL_CF        RADIO_REG(0xDFCF)
#define RADIO_CAL_D0        RADIO_REG(0xDFD0)
#define RADIO_CAL_D1        RADIO_REG(0xDFD1)
#define RADIO_CAL_D2        RADIO_REG(0xDFD2)
#define RADIO_CAL_D3        RADIO_REG(0xDFD3)
#define RADIO_CAL_D4        RADIO_REG(0xDFD4)
#define RADIO_CAL_D5        RADIO_REG(0xDFD5)
#define RADIO_UNK_D7        RADIO_REG(0xDFD7)
#define RADIO_UNK_D8        RADIO_REG(0xDFD8)
#define RADIO_UNK_E2        RADIO_REG(0xDFE2)
#define RADIO_UNK_F0        RADIO_REG(0xDFF0)
#define RADIO_POWER_CTL     RADIO_REG(0xDFF4)
#define RADIO_CH_SETTING1   RADIO_REG(0xDFFD)
#define RADIO_SLEEP_HI      RADIO_REG(0xDFD9)   /* sleep timer, 20 bits, counts down */
#define RADIO_SLEEP_MID     RADIO_REG(0xDFDA)
#define RADIO_SLEEP_LO      RADIO_REG(0xDFDB)
#define RADIO_SLEEP_CFG     RADIO_REG(0xDFF3)   /* 0x16: 1 s ticks, 0x56: 1/32768 s ticks; | 0x80 to arm */
#define RADIO_WAKE_REASON   RADIO_REG(0xDFFB)   /* 0x08 timer, 0x04 pin, 0x02 radio */

#define RADIO_CMD_RECEIVE   0xC2
#define RADIO_CMD_FLUSH_RX  0xC6
#define RADIO_CMD_FLUSH_TX  0xC7
#define RADIO_CMD_LOAD_TX   0xCB

/* Radio extra (radio_zbs243.c): the PAN ID for the frame filter. Default CFG_PAN_ID. */
void radio_set_pan(uint16_t pan);

#endif /* ZBS243_H */
