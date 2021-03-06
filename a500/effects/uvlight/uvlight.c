#include "startup.h"
#include "blitter.h"
#include "coplist.h"
#include "interrupts.h"
#include "memory.h"
#include "io.h"
#include "color.h"
#include "png.h"

STRPTR __cwdpath = "data";

#define WIDTH 80
#define HEIGHT 64
#define DEPTH 4
#define OPTIMIZED 1

static BitmapT *screen[2];
static UWORD active = 0;
static UWORD *shademap;
static UWORD *chunky[2];
static CopListT *cp;
static CopInsT *bplptr[DEPTH];
static PixmapT *uvmap;
static PixmapT *light;
static UWORD *texture;

#if OPTIMIZED
#define UVMapRenderSize (WIDTH * HEIGHT * 8 + 2)
static void (*UVMapRender)(UWORD *chunky asm("a0"), UWORD *texture asm("a1"),
                           UWORD *shades asm("a2"));

static void MakeUVMapRenderCode() {
  UWORD *code = (APTR)UVMapRender;
  UWORD *data = uvmap->pixels;
  UBYTE *lmap = light->pixels;
  WORD n = WIDTH * HEIGHT;

  while (--n >= 0) {
    UWORD x = *data++;
    UWORD y = *lmap++;
    if (x & 1) {
      *code++ = 0x5488; /* 5488      | addq.l #2,%a0 */
    } else {
      *code++ = 0x3029;
      *code++ = x;      /* 3029 xxxx | move.w xxxx(a1),d0 */
      *code++ = 0x30f2;
      *code++ = y;      /* 30f2 00yy | move.w yy(a2,d0.w),(a0)+ */
    }
  }

  *code++ = 0x4e75; /* rts */
}
#endif

static UWORD bluetab[16] = {
  0x0000, 0x0003, 0x0030, 0x0033, 0x0300, 0x0303, 0x0330, 0x0333,
  0x3000, 0x3003, 0x3030, 0x3033, 0x3300, 0x3303, 0x3330, 0x3333,
};

static UWORD greentab[16] = {
  0x0000, 0x0004, 0x0040, 0x0044, 0x0400, 0x0404, 0x0440, 0x0444,
  0x4000, 0x4004, 0x4040, 0x4044, 0x4400, 0x4404, 0x4440, 0x4444,
};

static UWORD redtab[16] = {
  0x0000, 0x0008, 0x0080, 0x0088, 0x0800, 0x0808, 0x0880, 0x0888,
  0x8000, 0x8008, 0x8080, 0x8088, 0x8800, 0x8808, 0x8880, 0x8888,
};

static void DataScramble(UWORD *data, WORD n) {
  UBYTE *in = (UBYTE *)data;
  UWORD *out = data;

  while (--n >= 0) {
    WORD ri = *in++;
    WORD gi = *in++;
    WORD bi = gi;

    /* [-- -- -- -- 11 10  9  8  7  6  5  4  3  2  1  0] */
    /* [-- -- -- -- r0 r1 r2 r3 g0 g1 g2 g3 b0 b1 b2 b3] */
    /* [11  7  3  3 10  6  2  2  9  5  1  1  8  4  0  0] */
    /* [r0 g0 b0 b0 r1 g1 b1 b1 r2 g2 b2 b2 r3 g3 b3 b3] */

    gi >>= 4;
    bi &= 15;

    *out++ = getword(redtab, ri) + getword(greentab, gi) + getword(bluetab, bi);
  }
}

static void Load() {
  PixmapT *image = LoadPNG("texture.png", PM_CMAP8, MEMF_PUBLIC);

  uvmap = LoadPNG("torus-map.png", PM_GRAY16, MEMF_PUBLIC);

  light = LoadPNG("torus-light.png", PM_GRAY8, MEMF_PUBLIC);
  {
    UBYTE *src = light->pixels;
    UBYTE *dst = light->pixels;
    WORD n = WIDTH * HEIGHT;

    while (--n >= 0)
      *dst++ = (*src++ >> 2) & 0x3E;
  }

  shademap = MemAlloc(32 * sizeof(UWORD) * image->palette->count, MEMF_PUBLIC);
  {
    ColorT *c = image->palette->colors;
    UWORD *dst = shademap;
    WORD n = image->palette->count;
    WORD i;

    while (--n >= 0) {
      for (i = 0; i < 16; i++)
        *dst++ = ColorTransitionRGB(0, 0, 0, c->r, c->g, c->b, i);
      for (i = 0; i < 16; i++)
        *dst++ = ColorTransitionRGB(c->r, c->g, c->b, 255, 255, 255, i);
      c++;
    }
  }
  DataScramble(shademap, image->palette->count * 32);

  texture = MemAlloc(128 * 128 * 2 * sizeof(UWORD), MEMF_PUBLIC);
  {
    UBYTE *src = image->pixels;
    UWORD *dst = texture;
    WORD n = 128 * 128;

    while (--n >= 0)
      *dst++ = *src++ << 6;

    /* Extra half for cheap texture motion. */
    memcpy((APTR)texture + 32768, texture, 32768);
  }

  DeletePixmap(image);
}

static void UnLoad() {
  DeletePixmap(light);
  DeletePixmap(uvmap);
  MemFree(texture);
}

static struct {
  WORD phase;
  APTR *bpl;
  APTR chunky;
} c2p = { 256, NULL };

#define BPLSIZE ((WIDTH * 4) * HEIGHT / 8) /* 2560 bytes */
#define BLTSIZE ((WIDTH * 4) * HEIGHT / 2) /* 10240 bytes */

static void ChunkyToPlanar() {
  APTR src = c2p.chunky;
  APTR dst = c2p.chunky + BLTSIZE;
  APTR *bpl = c2p.bpl;

  switch (c2p.phase) {
    case 0:
      /* Initialize chunky to planar. */
      custom->bltamod = 4;
      custom->bltbmod = 4;
      custom->bltdmod = 4;
      custom->bltcdat = 0x00FF;
      custom->bltafwm = -1;
      custom->bltalwm = -1;

      /* Swap 8x4, pass 1. */
      custom->bltapt = src + 4;
      custom->bltbpt = src;
      custom->bltdpt = dst;

      /* ((a >> 8) & 0x00FF) | (b & ~0x00FF) */
      custom->bltcon0 = (SRCA | SRCB | DEST) | (ABC | ANBC | ABNC | NABNC) | (8 << ASHIFTSHIFT);
      custom->bltcon1 = 0;
      custom->bltsize = 2 | ((BLTSIZE / 16) << 6);
      break;

    case 1:
      custom->bltsize = 2 | ((BLTSIZE / 16) << 6);
      break;

    case 2:
      /* Swap 8x4, pass 2. */
      custom->bltapt = src + BLTSIZE - 6;
      custom->bltbpt = src + BLTSIZE - 2;
      custom->bltdpt = dst + BLTSIZE - 2;

      /* ((a << 8) & ~0x00FF) | (b & 0x00FF) */
      custom->bltcon0 = (SRCA | SRCB | DEST) | (ABNC | ANBNC | ABC | NABC) | (8 << ASHIFTSHIFT);
      custom->bltcon1 = BLITREVERSE;
      custom->bltsize = 2 | ((BLTSIZE / 16) << 6);
      break;

    case 3:
      custom->bltsize = 2 | ((BLTSIZE / 16) << 6);
      break;

    case 4:
      custom->bltamod = 6;
      custom->bltbmod = 6;
      custom->bltdmod = 0;
      custom->bltcdat = 0x0F0F;

      custom->bltapt = dst + 2;
      custom->bltbpt = dst;
      custom->bltdpt = bpl[0];

      /* ((a >> 4) & 0x0F0F) | (b & ~0x0F0F) */
      custom->bltcon0 = (SRCA | SRCB | DEST) | (ABC | ANBC | ABNC | NABNC) | (4 << ASHIFTSHIFT);
      custom->bltcon1 = 0;
      custom->bltsize = 1 | ((BLTSIZE / 16) << 6);
      break;

    case 5:
      custom->bltsize = 1 | ((BLTSIZE / 16) << 6);
      break;

    case 6:
      custom->bltapt = dst + 6;
      custom->bltbpt = dst + 4;
      custom->bltdpt = bpl[2];
      custom->bltsize = 1 | ((BLTSIZE / 16) << 6);
      break;

    case 7:
      custom->bltsize = 1 | ((BLTSIZE / 16) << 6);
      break;

    case 8:
      custom->bltapt = dst + BLTSIZE - 8;
      custom->bltbpt = dst + BLTSIZE - 6;
      custom->bltdpt = bpl[1] + BPLSIZE - 2;

      /* ((a << 8) & ~0x0F0F) | (b & 0x0F0F) */
      custom->bltcon0 = (SRCA | SRCB | DEST) | (ABNC | ANBNC | ABC | NABC) | (4 << ASHIFTSHIFT);
      custom->bltcon1 = BLITREVERSE;
      custom->bltsize = 1 | ((BLTSIZE / 16) << 6);
      break;

    case 9:
      custom->bltsize = 1 | ((BLTSIZE / 16) << 6);
      break;

    case 10:
      custom->bltapt = dst + BLTSIZE - 4;
      custom->bltbpt = dst + BLTSIZE - 2;
      custom->bltdpt = bpl[3] + BPLSIZE - 2;
      custom->bltsize = 1 | ((BLTSIZE / 16) << 6);
      break;

    case 11:
      custom->bltsize = 1 | ((BLTSIZE / 16) << 6);
      break;

    case 12:
      CopInsSet32(bplptr[0], bpl[3]);
      CopInsSet32(bplptr[1], bpl[2]);
      CopInsSet32(bplptr[2], bpl[1]);
      CopInsSet32(bplptr[3], bpl[0]);
      break;

    default:
      break;
  }

  c2p.phase++;

  custom->intreq = INTF_BLIT;
}

INTERRUPT(ChunkyToPlanarInterrupt, 0, ChunkyToPlanar, NULL);

static struct Interrupt *oldBlitInt;

static void MakeCopperList(CopListT *cp) {
  WORD i;

  CopInit(cp);
  CopSetupGfxSimple(cp, MODE_HAM, 7, X(0), Y(0), WIDTH * 4 + 2, HEIGHT * 4);
  CopSetupBitplanes(cp, bplptr, screen[active], DEPTH);
  CopMove16(cp, bpldat[4], 0x7777); // rgbb: 0111
  CopMove16(cp, bpldat[5], 0xcccc); // rgbb: 1100
  CopLoadColor(cp, 0, 15, 0);
  for (i = 0; i < HEIGHT * 4; i++) {
    CopWaitSafe(cp, Y(i), 0);
    /* Line quadrupling. */
    CopMove16(cp, bpl1mod, ((i & 3) != 3) ? -40 : 0);
    CopMove16(cp, bpl2mod, ((i & 3) != 3) ? -40 : 0);
    /* Alternating shift by one for bitplane data. */
    CopMove16(cp, bplcon1, (i & 1) ? 0x0022 : 0x0000);
  }
  CopEnd(cp);
}

static void Init() {
  screen[0] = NewBitmap(WIDTH * 4, HEIGHT, DEPTH);
  screen[1] = NewBitmap(WIDTH * 4, HEIGHT, DEPTH);

  chunky[0] = MemAlloc((WIDTH * 4) * HEIGHT, MEMF_CHIP);
  chunky[1] = MemAlloc((WIDTH * 4) * HEIGHT, MEMF_CHIP);

#if OPTIMIZED
  UVMapRender = MemAlloc(UVMapRenderSize, MEMF_PUBLIC);
  MakeUVMapRenderCode();
#endif

  EnableDMA(DMAF_BLITTER);

  BitmapClear(screen[0]);
  BitmapClear(screen[1]);

  cp = NewCopList(1200);
  MakeCopperList(cp);
  CopListActivate(cp);

  EnableDMA(DMAF_RASTER);

  oldBlitInt = SetIntVector(INTB_BLIT, ChunkyToPlanarInterrupt);
  EnableINT(INTF_BLIT);
}

static void Kill() {
  DisableDMA(DMAF_COPPER | DMAF_RASTER);

  DisableINT(INTF_BLIT);
  SetIntVector(INTB_BLIT, oldBlitInt);

  DeleteCopList(cp);
#if OPTIMIZED
  MemFree(UVMapRender);
#endif

  MemFree(chunky[0]);
  MemFree(chunky[1]);

  DeleteBitmap(screen[0]);
  DeleteBitmap(screen[1]);
}

static void Render() {
  LONG lines = ReadLineCounter();
#if OPTIMIZED
  (*UVMapRender)(chunky[active], &texture[frameCount & 16383], shademap);
#else
  {
    APTR shade = shademap;
    APTR tex = &texture[frameCount & 16383];
    UWORD *data = uvmap->pixels;
    UBYTE *lmap = light->pixels;
    UWORD *dst = chunky[active];
    WORD n = WIDTH * HEIGHT;

    while (--n >= 0) {
      UWORD d = *data;
      if (d & 1) {
        data++;
        lmap++;
        dst++;
      } else {
        WORD c = *(WORD *)(tex + *data++) | *lmap++;
        *dst++ = *(UWORD *)(shade + c);
      }
    }
  }
#endif
  Log("uvlight: %ld\n", ReadLineCounter() - lines);

  c2p.phase = 0;
  c2p.chunky = chunky[active];
  c2p.bpl = screen[active]->planes;
  ChunkyToPlanar();
  active ^= 1;
}

EffectT Effect = { Load, UnLoad, Init, Kill, Render };
