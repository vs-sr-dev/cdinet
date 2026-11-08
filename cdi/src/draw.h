/* cdiNet - text and boxes on the screen: 6 x 10 character cells */
#ifndef CN_DRAW_H
#define CN_DRAW_H

#define CELL_W 6
#define CELL_H 10

#define A_BOLD      0x01
#define A_UNDERLINE 0x02

void fillRect();      /* x, y, w, h, colour */
void drawChar();      /* x, y, character, ink, paper, attributes */
int  drawText();      /* x, y, text, ink, paper, attributes, at most n chars; returns x after */

#endif
