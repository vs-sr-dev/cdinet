/* cdiNet - the serial line (see link.h).
 *
 * /t2 is the SCC68070's UART through the BIOS's u68070 driver. Its SCF path options are
 * a terminal's: echo, line editing, a pause key, ^C and ^E as signals, XON/XOFF. They are
 * all turned off, or bytes of our frames would be eaten or would signal us. The driver
 * keeps 128 bytes; we read whatever is there once a field, and never have more than one
 * reply (at most 106 bytes) coming. */
#include <stdio.h>
#include <sgstat.h>
#include <sysio.h>
#include "video.h"
#include "link.h"

#define SYN 0x16
/* OS-9's code for the line speed: 0E = 9600, 0F = 19200, the fastest the u68070 driver
 * knows (its table at 416848 in the 220 F2 ROM). The proxy does not care: TCP. */
#define LINK_BAUD 0x0f
#define RESEND_FIELDS 100       /* 2 s in PAL */
#define TRIES 5

int replyType;
unsigned char replyData[LINK_MAX_PAYLOAD];
int replyLen;
int linkTries;

static int path = -1;
static unsigned char out[LINK_MAX_PAYLOAD + 6];
static int outLen;
static int seq;
static int waiting;
static unsigned int sentAt;

/* the receiver: hunting for SYN SYN, then type, length, payload, CRC */
static int rxState;
static unsigned char rxBuf[LINK_MAX_PAYLOAD + 4];
static int rxLen, rxNeed;

static unsigned short crc16(p, n)
	register unsigned char *p;
	register int n;
{
	register unsigned short crc = 0xffff;
	register int i;

	while (n-- > 0) {
		crc ^= (unsigned short)*p++ << 8;
		for (i = 0; i < 8; i++)
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
	}
	return crc;
}

int linkOpen()
{
	struct sgbuf opt;

	path = open("/t2", UPDAT_);
	if (path < 0)
		return 0;
	_gs_opt(path, &opt);
	opt.sg_echo = 0;
	opt.sg_alf = 0;
	opt.sg_pause = 0;
	opt.sg_backsp = 0;
	opt.sg_bspch = 0;
	opt.sg_dlnch = 0;
	opt.sg_eorch = 0;
	opt.sg_eofch = 0;
	opt.sg_rlnch = 0;
	opt.sg_dulnch = 0;
	opt.sg_psch = 0;
	opt.sg_kbich = 0;
	opt.sg_kbach = 0;
	opt.sg_xon = 0;
	opt.sg_xoff = 0;
	opt.sg_baud = LINK_BAUD;
	_ss_opt(path, &opt);
	return 1;
}

static void send()
{
	write(path, out, outLen);
	sentAt = frameTick;
	linkTries++;
}

void linkRequest(type, args, n)
	int type;
	unsigned char *args;
	int n;
{
	unsigned short crc;
	int i;

	if (n > LINK_MAX_PAYLOAD - 1)
		n = LINK_MAX_PAYLOAD - 1;
	seq = seq % 255 + 1;
	out[0] = SYN;
	out[1] = SYN;
	out[2] = type;
	out[3] = n + 1;
	out[4] = seq;
	for (i = 0; i < n; i++)
		out[5 + i] = args[i];
	crc = crc16(out + 2, n + 3);
	out[5 + n] = crc >> 8;
	out[6 + n] = crc;
	outLen = n + 7;
	waiting = 1;
	linkTries = 0;
	send();
}

int linkBusy()
{
	return waiting;
}

/* one received byte; 1 when it completes a good frame */
static int rxByte(c)
	int c;
{
	switch (rxState) {
	case 0:                     /* the first SYN */
		if (c == SYN)
			rxState = 1;
		return 0;
	case 1:                     /* the second */
		rxState = c == SYN ? 2 : 0;
		return 0;
	case 2:                     /* type (a third SYN: still hunting) */
		if (c == SYN)
			return 0;
		rxBuf[0] = c;
		rxState = 3;
		return 0;
	case 3:                     /* length */
		if (c > LINK_MAX_PAYLOAD) {
			rxState = 0;
			return 0;
		}
		rxBuf[1] = c;
		rxLen = 2;
		rxNeed = c + 4;         /* type, len, payload, CRC */
		rxState = 4;
		return 0;
	}
	rxBuf[rxLen++] = c;
	if (rxLen < rxNeed)
		return 0;
	rxState = 0;
	return crc16(rxBuf, rxNeed - 2) == ((rxBuf[rxNeed - 2] << 8) | rxBuf[rxNeed - 1]);
}

int linkPoll()
{
	unsigned char buf[128];
	int n, i, k;

	if (path < 0)
		return 0;
	n = _gs_rdy(path);
	if (n > 0) {
		if (n > sizeof(buf))
			n = sizeof(buf);
		n = read(path, buf, n);
		for (i = 0; i < n; i++) {
			if (!rxByte(buf[i]))
				continue;
			/* a good frame: is it the reply to what we asked? */
			if (!waiting || rxBuf[1] < 1 || rxBuf[2] != seq)
				continue;
			replyType = rxBuf[0];
			replyLen = rxBuf[1] - 1;
			for (k = 0; k < replyLen; k++)
				replyData[k] = rxBuf[3 + k];
			waiting = 0;
			rxState = 0;        /* anything after it is noise */
			return 1;
		}
	}
	if (waiting && frameTick - sentAt > RESEND_FIELDS) {
		if (linkTries >= TRIES) {
			waiting = 0;
			return -1;
		}
		send();
	}
	return 0;
}
