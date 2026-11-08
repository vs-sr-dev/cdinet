/* cdiNet - the serial line to the proxy (docs/protocol.md): one request at a time,
 * resent until its reply comes */
#ifndef CN_LINK_H
#define CN_LINK_H

#define LINK_MAX_PAYLOAD 100

/* the reply, once linkPoll has said so: its type and payload (after the sequence number) */
extern int replyType;
extern unsigned char replyData[LINK_MAX_PAYLOAD];
extern int replyLen;
extern int linkTries;           /* sends of the current request so far */

int  linkOpen();                /* /t2 in raw mode; 0 if it cannot be opened */
void linkRequest();             /* type, args, n: send a new request */
int  linkPoll();                /* 1: a reply came; -1: none after 5 tries; 0: waiting/idle */
int  linkBusy();

#endif
