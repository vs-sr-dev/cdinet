/* cdiNet - pictures (see picture.h).
 *
 * The proxy packs a picture's palette indices with LZSS (proxy/cdinet_proxy.py, lzss):
 * a flag byte before each 8 items, bit 0 first; 0 = a literal byte, 1 = two bytes
 * holding distance-1 (12 bits) and length-3 (4 bits) of a copy from the last 4096
 * pixels. The decoder takes one byte at a time, so a picture is drawn line by line while
 * the serial line brings it; the copies come from a ring of the pixels already out. */
#include "video.h"
#include "picture.h"

static unsigned char ring[4096];
static int ringPos;
static int flags, flagBits, hi;
static unsigned char *row;
static int x, w;
static long left;

void pictureStart(px, py, pw, ph)
	int px, py, pw, ph;
{
	row = screen + py * SCREEN_WIDTH + px;
	x = 0;
	w = pw;
	left = (long)pw * ph;
	ringPos = 0;
	flagBits = 0;
	hi = -1;
}

static void pixel(c)
	register int c;
{
	ring[ringPos++ & 4095] = c;
	if (left <= 0)
		return;
	row[x] = c + C_PICTURE;
	if (++x == w) {
		x = 0;
		row += SCREEN_WIDTH;
	}
	left--;
}

void pictureByte(b)
	register int b;
{
	register int d, n;

	b &= 0xff;
	if (flagBits == 0) {
		flags = b;
		flagBits = 8;
		return;
	}
	if (flags & 1) {
		if (hi < 0) {
			hi = b;
			return;
		}
		d = ((hi << 4) | (b >> 4)) + 1;
		for (n = (b & 15) + 3; n > 0; n--)
			pixel(ring[(ringPos - d) & 4095]);
		hi = -1;
	} else
		pixel(b);
	flags >>= 1;
	flagBits--;
}

int pictureLeft()
{
	return left > 0 ? (int)(left > 32767 ? 32767 : left) : 0;
}
