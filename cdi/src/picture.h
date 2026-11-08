/* cdiNet - pictures: LZSS-packed CLUT indices, drawn as the bytes come in */
#ifndef CN_PICTURE_H
#define CN_PICTURE_H

void pictureStart();    /* x, y, w, h: where on the screen */
void pictureByte();     /* the next byte of LZSS data */
int  pictureLeft();     /* pixels still to come */

#endif
