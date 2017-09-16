#include "startup.h"
#include "blitter.h"
#include "coplist.h"
#include "memory.h"
#include "io.h"
#include "png.h"
#include "random.h"
#include "fx.h"
#include "tasks.h"

STRPTR __cwdpath = "data";

#define WIDTH (320 - 32)
#define HEIGHT 256
#define DEPTH 0

#define HTILES (WIDTH / 8)
#define VTILES (HEIGHT / 4)

#undef X
#undef Y

/* Don't change these settings without reading a note about copper chunky! */
#define Y(y) ((y) + 0x2c)
#define VP(y) (Y(y) & 255)
#define X(x) ((x) + 0x88)
#define HP(x) (X(x) / 2)

static CopInsT *chunky[2][VTILES];
static CopListT *cp[2];
static PixmapT *colors;
static LONG active = 0;

static BYTE tab1[256], tab2[256], tab3[256];
static UBYTE a0, a1, a2, a3, a4;
static BYTE xbuf[HTILES], ybuf[VTILES];

static void GeneratePlasmaTables() {
  WORD i;

  /* Generate plasma tables */
  for (i = 0; i < 256; i++) {
    WORD rad = i * 16;

    tab1[i] = fx4i(3 * 47) * SIN(rad * 2) >> 16;
    tab2[i] = fx4i(3 * 31) * COS(rad * 2) >> 16;
    tab3[i] = fx4i(3 * 37) * SIN(rad * 2) >> 16;
  }
}

static void Load() {
  colors = LoadPNG("plasma-colors.png", PM_RGB12, MEMF_PUBLIC);

  GeneratePlasmaTables();
}

static void UnLoad() {
  DeletePixmap(colors);
}

/* Double-buffered copper chunky with 8x4 pixels.
 *
 * In order to make copper run same stream of instructions (i.e. color line)
 * 4 times, the screen has to be constructed in a very specific way.
 *
 * Firstly, vertical start position must be divisible by 4. This is driven by
 * copper lack of capability to mask out the most significant bit in WAIT and
 * SKIP instruction. This causes a glitch after transition from line 127 to
 * line 128. The problem is mentioned in HRM:
 * http://amigadev.elowar.com/read/ADCD_2.1/Hardware_Manual_guide/node005B.html
 *
 * Secondly, a problem arises when transitioning from line 255 to line 256.
 * Inserted SKIP instruction effectively awaits beam position that won't ever
 * happen! To avoid it each SKIP instruction must be guaranteed to eventually
 * trigger. Thus they must be placed just before the end of scan line.
 * UAE copper debugger facility was used to find the right spot.
 */
static void MakeCopperList(CopListT *cp, CopInsT **row) {
  WORD x, y;

  CopInit(cp);
  CopWaitV(cp, VP(0));

  for (y = 0; y < VTILES; y++) {
    CopInsT *location = CopMove32(cp, cop2lc, 0);
    CopInsT *label = CopWaitH(cp, VP(y * 4), HP(-4));
    CopInsSet32(location, label);
    row[y] = CopSetRGB(cp, 0, 0);
    for (x = 0; x < HTILES - 1; x++)
      CopSetRGB(cp, 0, 0); /* Last CopIns finishes at HP=0xD2 */
    CopSetRGB(cp, 0, 0); /* set background to black, finishes at HP=0xD6 */
    CopSkip(cp, VP(y * 4 + 3), 0xDE); /* finishes at HP=0xDE */
    CopMove16(cp, copjmp2, 0);
  }

  CopEnd(cp);
}

static void Init() {
  cp[0] = NewCopList(80 + (HTILES + 5) * VTILES);
  cp[1] = NewCopList(80 + (HTILES + 5) * VTILES);

  MakeCopperList(cp[0], chunky[0]);
  MakeCopperList(cp[1], chunky[1]);

  CopListActivate(cp[1]);
}

static void Kill() {
  DisableDMA(DMAF_COPPER);

  DeleteCopList(cp[0]);
  DeleteCopList(cp[1]);
}

static void UpdateXBUF() {
  UBYTE _a0 = a0;
  UBYTE _a1 = a1;
  UBYTE _a2 = a2;
  UBYTE *_buf = xbuf;
  WORD n = HTILES - 1;

  do {
    *_buf++ = tab1[_a0] + tab2[_a1] + tab3[_a2];
    _a0 += 1; _a1 += 2; _a2 += 3;
  } while (--n >= 0);
}

static void UpdateYBUF() {
  UBYTE _a3 = a3;
  UBYTE _a4 = a4;
  UBYTE *_buf = ybuf;
  WORD n = VTILES - 1;

  do {
    *_buf++ = tab1[_a3] + tab2[_a4];
    _a3 += 2; _a4 += 3;
  } while (--n >= 0);
}

#define OPTIMIZED 1

static void UpdateChunky() {
  UWORD *cmap = colors->pixels;
  WORD y;

  UpdateXBUF();
  UpdateYBUF();

  a0 += 1; a1 += 3; a2 += 2; a3 += 1; a4 -= 1;

  for (y = 0; y < VTILES; y++) {
    UWORD *ins = &chunky[active][y]->move.data;

    WORD x = HTILES - 1;
    do {
#if OPTIMIZED
      asm volatile("moveq #0,d0\n"
                   "moveb %1@(%2:w),d0\n"
                   "addb  %3@(%4:w),d0\n"
                   "addw  d0,d0\n"
                   "movew %5@(d0:w),%0@+\n"
                   "addql #2,%0\n"
                   : "+a" (ins)
                   : "a" (xbuf), "d" (x), "a" (ybuf), "d" (y), "a" (cmap)
                   : "d0");
#else
      UBYTE v = xbuf[x] + ybuf[y];
      *ins++ = cmap[v];
      ins++;
#endif
    } while (--x >= 0);
  }
}

static void Render() {
  LONG lines;

  lines = ReadLineCounter();
  UpdateChunky();
  Log("update: %ld\n", ReadLineCounter() - lines);

  CopListRun(cp[active]);
  TaskWait(VBlankEvent);
  active ^= 1;
}

EffectT Effect = { Load, UnLoad, Init, Kill, Render };
