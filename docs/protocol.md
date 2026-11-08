# cdiNet line protocol, version 1

The CD-i and the proxy talk over the player's serial port (OS-9 `/t2`, 19200 baud, 8N1;
in cdiPC, `--serial HOST:PORT`). The proxy does everything the web needs (TLS, HTML,
search); the CD-i shows screens of text and links and sends back what the user picked.

## Why it is shaped like this

- **The BIOS's u68070 driver keeps 128 bytes.** A program busy drawing for half a second
  at 9600 baud would lose data if the proxy streamed. So the CD-i only ever has one frame
  in flight: every CD-i message is a request, every request gets exactly one reply, and
  a reply is at most 106 bytes.
- **The line is not clean.** The player shell writes to `/t2` before our program runs
  (a `01` byte), and a real cable can drop or corrupt bytes. Frames start with two SYN
  bytes, carry a CRC, and the CD-i resends a request that gets no good reply in 2 s.
- **Resending must be harmless.** Requests carry a sequence number; the proxy answers a
  repeated number with its stored reply instead of acting twice (following a link twice).

## Frames

```
16 16 | type | len | payload (len bytes, at most 100) | crc hi | crc lo
```

`crc` is CRC-16/CCITT-FALSE (poly 1021, start FFFF) over `type`, `len` and the payload.
A receiver hunts for `16 16`, and drops a frame whose CRC is wrong. Numbers are big-endian.

Every payload starts with `seq` (1 byte): the CD-i counts its requests 1-255 (then 1
again); a reply carries the number of the request it answers.

## Requests (CD-i -> proxy)

| type | payload after seq | meaning |
|------|-------------------|---------|
| `H`  | version 1, cols, rows | hello: the text area is cols x rows characters |
| `G`  | text | go: a URL, or words to search for; empty = the home page |
| `L`  | link id (2) | follow a link of the current page |
| `B`  | - | back |
| `P`  | page (2) | show another screen of the current page (0 = first) |
| `N`  | offset (2) | send the screen from this byte on |

`G`, `L`, `B` and `P` change what is shown. Their reply is a `W` (the proxy is working:
loading takes seconds) or the first chunk of the new screen. After a `W` the CD-i asks
`N 0` every half second until a chunk comes.

## Replies (proxy -> CD-i)

| type | payload after seq | meaning |
|------|-------------------|---------|
| `h`  | version 1, name | hello back |
| `W`  | - | working, ask again |
| `C`  | screen id, offset (2), total (2), data | a chunk of the screen (up to 94 bytes) |
| `E`  | text | an error the CD-i shows on its status line |

The screen id (1-255) changes with every new screen, so the CD-i can tell stale chunks
from those it asked for.

## The screen

```
page (2) | pages (2) | title len (1) | title | url len (1) | url | rows (1) | row...
row: len (1) | bytes
```

Text is ISO 8859-1. In a row, bytes `20`-`7E` and `A0`-`FF` are characters, and:

| byte | meaning |
|------|---------|
| `01` id(2) | a link starts (its id counts from 1 within the page, across all screens) |
| `02` | the link ends |
| `03` style(1) | from here: bit 0 bold, bit 1 heading colour, bit 2 quoted/preformatted colour |

A row holds at most `cols` characters; the proxy has wrapped the page already.

## A picture

When the row count is `FF` the screen is a picture instead of text:

```
... | rows = FF | width (2) | height (2) | colours n (1) | palette (n x R, G, B) | LZSS data
```

The palette goes to CLUT entries 16 to 16 + n - 1 (n at most 48, so bank 0 only). The
data is the picture's palette indices, row by row, packed with LZSS: a flag byte before
every 8 items, bit 0 first; a 0 bit is one literal byte, a 1 bit a copy of 3-18 bytes
from 1-4096 bytes back, in two bytes: distance - 1 (12 bits) then length - 3 (4 bits).
The CD-i decodes as the chunks come, so the picture appears line by line.

In a page, pictures are links shown as `[image: alt]` to `cdinet:image:URL`; following
one asks the proxy for that picture. A screen is at most 65535 bytes (16-bit offsets).

## Timing (cdiPC, 2026-11-08)

A 250 x 140 photo in 32 colours: 11.9 KB. Asking for the next chunk before decoding the
current one, at 19200 baud: 129 chunks in 15.6 s (122 ms each, the 68070 the limit).
At 9600 baud: 20 s; before the reordering, 28 s.
