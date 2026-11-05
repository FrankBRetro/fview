# FView 1.5 — GEM file viewer, FConv picture converter and FArchive archiver for TOS, MultiTOS and MiNT

FView (`fview.prg`) starts with its menu bar and an **iconified main window**. Drop files
on it (or on any fview window) from the desktop, open them with **File ›
Open**, or pass them on the command line / via VA_START. Every file opens in
its own window.

| Pictures | Documents |
|---|---|
| GIF (87a/89a, interlace, transparency) | Plain text (Atari, Latin-1 or CP1252) |
| PNG (all depths, palette, alpha) | Unicode text (UTF-8, UTF-16 LE/BE with or without BOM) |
| IFF ILBM/PBM (1–8 planes, EHB, HAM6, HAM8, 24/32-bit deep, masks, Deluxe Paint ST VDAT compression) | Markdown (headings, emphasis, code, lists, quotes, rules, links, tables) |
| GEM IMG / XIMG (1–8 planes, 24-bit) | RTF with GDOS fonts: font table, sizes, colours, alignment, page width |
| JPEG (baseline and progressive) | PDF: text (GDOS fonts by family), lines, fills and images; compressed object streams |
| BMP / DIB (1, 4, 8, 16, 24, 32 bit; RLE4, RLE8; bit fields; alpha; top-down; OS/2 headers) | GEM metafiles (.GEM): lines, fills and VDI patterns, boxes, circles, ellipses, arcs, pies, text, placed IMG pictures (Easy-Draw, GEM Draw, Artline) |

**Atari ST pictures**

| Format | Files |
|---|---|
| Degas / Degas Elite | `.PI1` `.PI2` `.PI3` (plain, incl. Elite's 32066-byte files and taller low-res sizes), `.PC1` `.PC2` `.PC3` (compressed) |
| NEOchrome | `.NEO`; NEOchrome Master's raster palettes from a `.RST` file of the same name, and its 640×400 pictures |
| Spectrum 512 | `.SPU` (plain), `.SPC` (compressed), `.SPS` (smooshed) |
| Photochrome | `.PCS`, including two-screen interlaced pictures (blended) |
| QuantumPaint | `.PBX`, all four modes (16 colours with palette changes, medium, 512 and 4096 colours), packed or plain |

Medium resolution pictures are shown with their tall pixels in proportion
(see *Pixel shape*). Multi-palette pictures are converted to true colour
and then dithered to the screen like any other picture.

## Building

Needs the MiNT cross toolchain with mintlib and gemlib:

```
make                    # everything: release and log builds for every CPU
make release            # release builds only
make VARIANTS=000       # only the 68000 builds (or 030, 030fpu, 040, cf)
```

| Build | Folder | Runs on |
|---|---|---|
| 68000 | `release/`, `log/` | every Atari |
| 68030 | `release/030/`, `log/030/` | TT, Falcon, 68030 accelerators (no FPU needed) |
| 68030 + 68882 | `release/030fpu/`, `log/030fpu/` | machines with an FPU: TT, Falcon or accelerator with a 68882. The PDF and GEM metafile renderers do floating point, which this build runs on the FPU |
| 68040 | `release/040/`, `log/040/` | 68040 machines and accelerators with the full 68040 (built-in FPU; not a 68LC040) |
| ColdFire | `release/cf/`, `log/cf/` | FireBee (ColdFire V4e with FPU) |

Each folder holds `fview.prg`, `fconv.prg` and `fvwork.ttp`, and
`farchive.prg` with `fawork.ttp` — **copy the programs of one build into
the same folder** (`fvwork.ttp` is the MiNT background worker FView and
FConv use, `fawork.ttp` FArchive's; without them the programs still work,
cooperatively). The log builds write `FVIEW.LOG` / `FCONV.LOG` /
`FARCHIVE.LOG` next to the program, see *Step log*. Do not build with `-mshort`. No resource file is
needed — menus, dialogs and icons are built at run time. PNG/JPEG decoding
uses `stb_image.h` (public domain, included in `src/`).

## FConv — picture converter

`fconv.prg` converts pictures to GEM IMG or PDF without showing them. It starts as
an icon (next to FView's); **drop pictures or whole folders on it** (or use
*File › Convert files...*, VA_START or the command line). Drops work both
the MultiTOS way (AP_DRAGDROP) and the AV way (VA_DRAGACCWIND, used by
Thing, Jinnee, TeraDesk and other AV desktops). As soon as something
arrives the icon opens into the log window. Every picture format FView
reads is accepted; other files are skipped (and listed in the log when
dropped on their own).

- **Format** menu: *GEM IMG files* or *PDF files* — a PDF is one page
  holding the picture at 72 dpi (non-square pixels kept in proportion),
  with its palette or as RGB, Flate compressed, so it opens in any PDF
  reader including FView. Then the depth: *Original depth* (the picture's own planes and palette,
  or 24-bit), *True colour* (24-bit), *Screen colours* (dithered to what the
  current screen mode shows — 16 or 256 colours, mono, or 24-bit on a true
  colour screen) or *Mono* (1 bit). *Fit to screen size* shrinks bigger
  pictures (not for original depth), *Replace existing files* overwrites
  instead of picking NAME1.IMG, NAME2.IMG..., *Include subfolders* for
  dropped folders. The source file is never overwritten.
- **Dither** menu: Floyd–Steinberg, Atkinson, ordered or none, serpentine,
  smooth shrink, and a palette per picture or the system palette.
- *File › Destination folder...* sets where the IMG files go (asked for on
  the first conversion otherwise). Settings are kept in `FCONV.INF` next to
  the program.
- While it works a non-modal dialog shows a bar for all files and one for
  the current file (reading, dithering, writing) with **Cancel**; the other
  windows stay usable. Under MiNT each file is converted by its own
  `fvwork.ttp` at nice 19.
- Open the icon to see the **log**: every file converted with the time it
  took, failures and a summary per batch (scrollable, last 200 lines). The
  same lines are appended to `FCONV.TXT` in the destination folder.

## FArchive — archiver

`farchive.prg` extracts and makes archives in **ZIP, LHA (.LZH), ZOO, ARC,
LZX, TAR, TAR.GZ (.TGZ) and GZ**. It works like FConv: it starts as an
icon (next to FConv's), takes drops the MultiTOS way (AP_DRAGDROP) and the
AV way (VA_DRAGACCWIND), VA_START and the command line, opens into the
same log window and shows the same non-modal progress dialog (all tasks,
the current task with the member being done, Cancel). Under MiNT each task
runs in its own `fawork.ttp` at nice 19; on TOS it runs inside FArchive.

- **Drop archives on the icon** to extract them into the destination
  folder (each into a folder of its own with *Folder per archive*), or —
  with *Options › Extract dropped archives* off — to open each in a window.
- **Drop other files or folders on the icon** to pack them into a new
  archive: FArchive asks for its name (the extension says the format; the
  *Format* menu gives the one for new archives).
- **Archive windows** (*File › Open archive...*, or *New archive...* for an
  empty one) list the members with size, packed size, method and date.
  Click to select, Shift-click to add or take away one, Control-click for
  everything from the last click; *File › Extract selected* (^X, nothing
  selected = all) or double-click a member to extract it. **Files and
  folders dropped on an archive window are added to it**, as is what
  *File › Add files...* picks (no name in the file selector = the folder).
  Members of the same name are replaced when *Replace existing files* is on.
- **8.3 names**: extracting to a FAT drive (TOS, or MiNT's FAT file
  systems — checked per drive with `Dpathconf`), member and folder names
  that are not valid 8.3 names are renamed (`Long File Name.txt` →
  `LONGFI~1.TXT`, clashes get `~2`, `~3`...) and each rename is listed in
  the log. Drives with long names (ext2, MinixFS...) get the names as they
  are. *Options › 8.3 names on FAT drives* switches this off.
- Paths in archives are made safe: drive letters and `..` are dropped.
  *Keep folders* off extracts everything flat into one folder.
- **The log** lists every task with what was extracted, added, skipped
  (already there and *Replace* off, links, unknown methods), renamed or
  failed (with CRC errors), and a summary per batch. It is also appended to
  `FARCHIVE.TXT` in the destination folder — **only on a hard disk**: when
  the destination is on A: or B: it goes next to the program instead, and
  when that is a floppy too, no log file is written.
- Settings are kept in `FARCHIVE.INF` next to the program.

| Format | Reads | Writes |
|---|---|---|
| ZIP | stored, deflate | deflate (stored for folders and empty files) |
| GZ, TAR.GZ | deflate, several members | deflate (*Best compression*: slower, a little smaller) |
| TAR | ustar, GNU long names, pax paths | ustar with GNU long names |
| LHA | -lh0- -lh1- -lh4- -lh5- -lh6- -lh7- -lzs- -lz4- -lz5- -lhd-, header levels 0–3 | -lh5- (level 1 headers, Atari LHarc style paths), -lh0- when it does not shrink |
| ZOO | stored, LZW, lh5 | LZW, stored when it does not shrink |
| ARC | stored, packed, squeezed, crunched (5–8), squashed | crunched (method 8), stored when it does not shrink; no folders (8.3 names) |
| LZX | stored, LZX (verbatim and aligned blocks, merged groups) | LZX, 64K window |

A GZ file holds a single file; use TAR.GZ for more. Adding to an archive
writes a new one next to it (`FARC$$$$.TMP`) and then replaces it, so
there must be room for both.

## Using it

| Key | Action |
|---|---|
| ^O / ^U / ^Q | Open, close window, quit |
| ^S / ^D | Save as IMG at original depth / as displayed (dithered) |
| ^P | Print (GDOS) |
| ^1 / ^2, double-click | Real size (PDF: 100%) / fit to window (PDF: fit width) |
| `+` / `-` | PDF zoom in / out (50% to 400%) |
| ^B / ^N, Clr/Home, Shift+Clr/Home | PDF previous / next / first / last page |
| Arrows, Shift+arrows, Space | Scroll by line / page |
| ^W | Cycle windows |
| ^E | Preferences |
| Help | About |

- **Loading**: while a window loads, re-renders, saves or prints, a small
  non-modal progress dialog of its own (bar, status, Cancel) sits over it;
  it uses the AES 3D look (sunken bar, 3D button) on TOS 4 / AES 3.40 and
  later. Everything stays usable meanwhile: the document window can be
  moved, sized or closed, other files opened. The busy bee appears only
  while the pointer is over a busy fview document window; fview never
  changes the pointer anywhere else. (If the AES has no window to spare,
  the progress panel is drawn inside the document window instead.)
- **MiNT**: with `U:\SHM` present, `fvwork.ttp` beside `fview.prg` and
  *MiNT workers* on, every load and re-render runs in its own `FVWORK`
  process (started with `Pexec` mode 100) at **nice 19**, so windows load
  concurrently while fview stays responsive. Results come back through
  shared memory, progress through a pipe. (`fork()` is not used: on
  machines without an MMU MiNT suspends the parent until a forked child
  exits.) The About box shows which mode is active. Under plain TOS (or with workers off) jobs run one at a
  time cooperatively, polling events so the desktop stays responsive.
- **Pixel shape**: pictures whose pixels weren't square are shown with
  their original proportions on the current screen mode. For Amiga IFF
  files the display mode (CAMG: lores/hires/superhires, interlace) and
  PAL or NTSC decide: a lores pixel is 10:11 on NTSC and 16:15 on PAL,
  so a full PAL or NTSC screen comes out 4:3. PAL/NTSC is taken from the
  monitor ID in CAMG, or else from the height (over 200 lines, 400
  interlaced, is PAL); the BMHD aspect fields are used without CAMG. GEM
  IMG files use the pixel size in their header. E.g. a PAL hires
  interlaced 640×512 picture is shown 4:3 (683×512), a PAL lores
  interlaced 352×556 overscan one 750×556 on a square-pixel screen. Stretching is limited to 2×; beyond that (ST medium's tall
  pixels) it also shrinks the other way. The info line says *aspect
  corrected*, and *Save IMG original* keeps the pixel shape in the header.
- **Colour**: pictures are quantised (median cut) and dithered
  (serpentine Floyd–Steinberg by default) to the display. On palette
  screens each window gets its own palette, set when it is topped; the
  system palette comes back when another program's window is on top.
  *Keep GEM colours* reserves white/black (≤16 colours) or the first 16
  pens (256 colours). True-colour screens (Falcon, graphics cards via
  EdDI/`vq_scrninfo`) get packed pixels directly.
- **Screen format check**: at start-up FView draws a few pens and colours
  on a one-pixel strip at the top of the screen (saved and put back
  straight away), copies them back and reads the bits. That gives the
  real pixel value of every pen, whether an 8-bit screen is chunky (one
  byte per pixel; FView then writes those bytes itself instead of using
  `vr_trnfm`), and where the red, green and blue bits sit in true colour,
  including byte-swapped (Intel order) 15/16-bit modes. What the driver
  reports is only used if this check can't run. The log build records both.
- **Saving**: *original* keeps the source depth (1/2/4/8-plane XIMG with
  palette, or 24-bit); *dithered* saves exactly what is on screen.
- **Printing**: needs a GDOS (GDOS, AMCGDOS, NVDI, SpeedoGDOS) with a
  printer driver on device 21 in `ASSIGN.SYS`. Pictures are dithered at
  printer resolution (fit to page or real size); text is laid out with the
  printer's system font; PDFs print the current page or all pages.

- **Fonts**: with a GDOS (SpeedoGDOS, NVDI, FontGDOS...) fview loads its
  fonts at start. RTF documents are laid out in their own fonts and sizes:
  the RTF font table names are matched to your GDOS fonts (exact face,
  then the same face in the wanted weight, then the same family: serif,
  sans, mono), with right/centre/justified alignment, colours and the
  page's text width. PDF text uses a GDOS font of the right family.
  Without GDOS everything uses the system font. Plain text and Markdown
  always use the system font.
- **PDF rendering**: a page's images are scaled and dithered once for each
  page and size. When that takes a while (opening, resizing the window,
  zooming, changing page) a progress dialog with Cancel shows it; the
  window stays usable. Cancelling shows boxes where the images go until
  the next resize, zoom or page change.
- **GEM metafiles with pictures**: Easy-Draw and similar programs don't
  copy a placed picture into the .GEM file; they store the name of its
  GEM IMG file. FView looks for that file in the same folder as the
  metafile first (as named, then in upper or lower case), then at the
  original path saved in the file. Keep the .IMG next to the .GEM. A
  picture it can't find shows as a crossed box.
- **PDF images**: grey, colour, indexed and 1-bit images and image masks
  (Flate with PNG predictors, RunLength, JPEG, ASCII hex/85) are shown,
  scaled and dithered to the screen and cached, so scrolling stays quick.
  1-bit images (scanned pages, and PDFs that store each page as a picture)
  are anti-aliased with the system palette's greys when shrunk, which keeps
  small text readable; mono screens and printers get a clean threshold.
  Rotated images still show as boxes. Up to 3 MB of image data is kept
  per document; switch *Images* off in Preferences for boxes only.

## Preferences

Non-modal window (Options › Preferences, ^E); **Apply** takes effect at
once (re-dithers open pictures, re-lays out PDFs), **Save** writes
`FVIEW.INF` to `$HOME`, or next to the program. Options: start iconified,
MiNT workers, dither method and serpentine, opening zoom, smooth shrink,
per-picture/system palette, keep GEM colours, transparency background,
GIF transparency, PNG alpha, IFF mask and HAM/EHB, IMG system palette and
XIMG pen order, JPEG grey on mono, tab width, wrapping, 8-bit text charset,
Markdown markup, RTF pictures, PDF zoom/graphics/image boxes, and print
size/dither/header. Format options apply to files opened afterwards.

## Step log

`make` produces two builds: `release/` without logging and `log/`, whose
`fview.prg` writes `FVIEW.LOG` next to itself (both programs append to it;
it is cleared at each start). Copy both programs of one build into the same
folder. `make release` or `make log` builds just one of them.

## Source

| File | |
|---|---|
| `main.c` | start-up, menus, event loop, drag & drop (`AP_DRAGDROP`), `VA_START`, keys, busy pointer |
| `win.c` | document windows, progress panel, scrolling, palettes, zoom, saving |
| `render.c` | text layout and PDF drawing, shared by screen and printer |
| `job.c` | starting/polling `fvwork.ttp` workers (Pexec, pipes, `U:\SHM`) and cooperative TOS jobs |
| `worker.c`, `jobcore.c` | the worker program (`fvwork.ttp`, `fawork.ttp`), result memory and the step log |
| `jobexec.c` | what an FView / FConv job does |
| `fconv.c` | FConv |
| `farchive.c`, `arcjob.c` | FArchive's GUI, and what its jobs do |
| `archive.c`, `arcfmt.c`, `arcfs.c` | archive formats: listing, extracting (8.3 names), adding; TAR/GZ in `archive.c` |
| `arcio.c`, `codecs.c` | byte and bit streams, CRCs, Huffman codes; which codec a method uses |
| `deflate.c`, `inflate.c`, `lzh.c`, `lzw.c`, `lzx.c` | the codecs: deflate, LHA's lh1–lh7/lzs/lz5, ZOO and ARC's LZW/RLE/squeeze, Amiga LZX |
| `print.c`, `prefs.c`, `tree.c` | GDOS printing, preferences, run-time object trees |
| `dec_*.c`, `dither.c`, `img_save.c` | decoders, quantiser/ditherer, IMG writer (no GEM code) |

## What has been tested, and what to check on the machine

No Atari toolchain or emulator was available where this was written, so:

- **Tested on a host** (`make hosttest`, `make rendertest GEMINC=…`): every
  decoder against sample files of each format, dithering at 1, 2, 4, 8 and
  16-bit depths, lossless IMG round trips (original and dithered), and the
  text/PDF renderers through a stub VDI, including a 33-page LaTeX PDF and
  a PDF using compressed object streams. Run under AddressSanitizer.
- **FArchive's archive layer tested on a host** (`make arctest`): archives
  it wrote were checked with Info-ZIP `unzip -t`, GNU tar and gzip, lhasa,
  ZOO 2.10, ARC 5.21 and an LZX extractor; archives from those tools (and
  lhasa's test archives from about 30 LHA programs, Atari LHarc 3.13
  included) extracted byte for byte; 8.3 renaming checked with a forced
  FAT mode. ARC's old methods 5–7 have no test files.
- **Type-checked for the 68000** against the real mintlib and gemlib
  headers (clang M68k backend, `-Wall`, no warnings).
- **Not yet run on TOS/MiNT.** Worth watching on first runs:
  shared-memory blocks surviving the worker's exit (`U:\SHM`), the bit
  layout reported by `vq_scrninfo` on graphics cards, XIMG palette order
  with other programs' files (switchable in Preferences), and GDOS printer
  drivers' handling of `vrt_cpyfm`.

Known limits: PNG decoding reports progress while reading but not during
decompression; PDF text is drawn in a GDOS font of the right family (or the
system font) fitted to each run's width, not in the embedded fonts;
encrypted PDFs, rotated images and shadings aren't shown.

## Licence

FView is released under the MIT License; see `LICENSE`.
Copyright (c) 2026 Francis C Bullen.

`src/stb_image.h` (PNG and JPEG decoding) is by Sean Barrett and
contributors and keeps its own licence (public domain or MIT, your
choice), stated at the end of that file.
