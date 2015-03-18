#include "startup.h"
#include "bltop.h"
#include "coplist.h"
#include "3d.h"
#include "fx.h"
#include "ffp.h"

#define WIDTH  256
#define HEIGHT 256
#define DEPTH  3

static Mesh3D *mesh;
static Object3D *cube;
static CopListT *cp;
static CopInsT *bplptr[DEPTH];
static BitmapT *screen0, *screen1;
static BitmapT *buffer;

static void Load() {
  mesh = LoadLWO("data/codi2.lwo", SPFlt(384));
  // mesh = LoadLWO("data/new_2.lwo", SPFlt(80));
  // mesh = LoadLWO("data/cube.lwo", SPFlt(50));
  CalculateFaceNormals(mesh);
  CalculateEdges(mesh);
}

static void UnLoad() {
  DeleteMesh3D(mesh);
}

static void MakeCopperList(CopListT *cp) {
  CopInit(cp);
  CopMakePlayfield(cp, bplptr, screen0, DEPTH);
  CopMakeDispWin(cp, X(32), Y(0), WIDTH, HEIGHT);
  CopSetRGB(cp, 0, 0x000);
  CopSetRGB(cp, 1, 0x333);
  CopSetRGB(cp, 2, 0x555);
  CopSetRGB(cp, 3, 0x777);
  CopSetRGB(cp, 4, 0x999);
  CopSetRGB(cp, 5, 0xBBB);
  CopSetRGB(cp, 6, 0xDDD);
  CopSetRGB(cp, 7, 0xFFF);
  CopEnd(cp);
}

static void Init() {
  cube = NewObject3D(mesh);
  cube->translate.z = fx4i(-250);

  screen0 = NewBitmap(WIDTH, HEIGHT, DEPTH);
  screen1 = NewBitmap(WIDTH, HEIGHT, DEPTH);
  buffer = NewBitmap(WIDTH, HEIGHT, 1);

  cp = NewCopList(80);
  MakeCopperList(cp);
  CopListActivate(cp);
  custom->dmacon = DMAF_SETCLR | DMAF_BLITTER | DMAF_RASTER;
}

static void Kill() {
  DeleteBitmap(screen0);
  DeleteBitmap(screen1);
  DeleteBitmap(buffer);
  DeleteCopList(cp);
  DeleteObject3D(cube);
}

static __regargs void UpdateVertexVisibility(Object3D *object) {
  BYTE *vertexFlags = object->vertexFlags;
  BYTE *faceFlags = object->faceFlags;
  IndexListT **faces = object->mesh->face;
  WORD n = object->mesh->faces;

  memset(vertexFlags, 0, object->mesh->vertices);

  while (--n >= 0) {
    IndexListT *face = *faces++;

    if (*faceFlags++) {
      WORD *vi = face->indices;
      WORD count = face->count;

      /* Face has at least (and usually) three vertices. */
      switch (count) {
        case 6: vertexFlags[*vi++] = -1;
        case 5: vertexFlags[*vi++] = -1;
        case 4: vertexFlags[*vi++] = -1;
        case 3: vertexFlags[*vi++] = -1;
                vertexFlags[*vi++] = -1;
                vertexFlags[*vi++] = -1;
        default: break;
      }
    }
  }
}

#define MULVERTEX1(D, E) {               \
  WORD t0 = (*v++) + y;                  \
  WORD t1 = (*v++) + x;                  \
  LONG t2 = (*v++) * z;                  \
  v++;                                   \
  D = ((t0 * t1 + t2 - x * y) >> 4) + E; \
}

#define MULVERTEX2(D) {                  \
  WORD t0 = (*v++) + y;                  \
  WORD t1 = (*v++) + x;                  \
  LONG t2 = (*v++) * z;                  \
  WORD t3 = (*v++);                      \
  D = normfx(t0 * t1 + t2 - x * y) + t3; \
}

static __regargs void TransformVertices(Object3D *object) {
  Matrix3D *M = &object->objectToWorld;
  WORD *v = (WORD *)M;
  WORD *src = (WORD *)object->mesh->vertex;
  WORD *dst = (WORD *)object->vertex;
  BYTE *flags = object->vertexFlags;
  register WORD n asm("d7") = object->mesh->vertices - 1;

  LONG m0 = (M->x << 8) - ((M->m00 * M->m01) >> 4);
  LONG m1 = (M->y << 8) - ((M->m10 * M->m11) >> 4);

  /* WARNING! This modifies camera matrix! */
  M->z -= normfx(M->m20 * M->m21);

  /*
   * A = m00 * m01
   * B = m10 * m11
   * C = m20 * m21 
   * yx = y * x
   *
   * (m00 + y) * (m01 + x) + m02 * z - yx + (mx - A)
   * (m10 + y) * (m11 + x) + m12 * z - yx + (my - B)
   * (m20 + y) * (m21 + x) + m22 * z - yx + (mz - C)
   */

  do {
    if (*flags++) {
      WORD x = *src++;
      WORD y = *src++;
      WORD z = *src++;
      LONG xp, yp;
      WORD zp;

      pushl(v);
      MULVERTEX1(xp, m0);
      MULVERTEX1(yp, m1);
      MULVERTEX2(zp);
      popl(v);

      *dst++ = div16(xp, zp) + WIDTH / 2;  /* div(xp * 256, zp) */
      *dst++ = div16(yp, zp) + HEIGHT / 2; /* div(yp * 256, zp) */
      *dst++ = zp;
    } else {
      src += 3;
      dst += 3;
    }
  } while (--n != -1);
}

#define MoveLong(reg, hi, lo) \
    *(ULONG *)(&custom->reg) = (((hi) << 16) | (lo))

static __regargs void DrawLine(WORD x0, WORD y0, WORD x1, WORD y1) {
  WORD dmax = x1 - x0;
  WORD dmin = y1 - y0;
  WORD derr;
  UWORD bltcon1 = LINE_ONEDOT;

  if (dmax < 0)
    dmax = -dmax;

  if (dmax >= dmin) {
    if (x0 >= x1)
      bltcon1 |= AUL;
    bltcon1 |= SUD;
  } else {
    if (x0 >= x1)
      bltcon1 |= SUL;
    swapr(dmax, dmin);
  }

  derr = 2 * dmin - dmax;
  if (derr < 0)
    bltcon1 |= SIGNFLAG;
  bltcon1 |= rorw(x0 & 15, 4);

  {
    APTR src = buffer->planes[0];
    WORD start = ((y0 << 5) + (x0 >> 3)) & ~1;
    APTR dst = src + start;
    UWORD bltcon0 = rorw(x0 & 15, 4) | LINE_EOR;
    UWORD bltamod = derr - dmax;
    UWORD bltbmod = 2 * dmin;
    UWORD bltsize = (dmax << 6) + 66;
    APTR bltapt = (APTR)(LONG)derr;

    WaitBlitter();

    custom->bltadat = 0x8000;
    custom->bltbdat = 0xffff; /* Line texture pattern. */
    custom->bltcon0 = bltcon0;
    custom->bltcon1 = bltcon1;
    custom->bltamod = bltamod;
    custom->bltbmod = bltbmod;
    custom->bltcmod = WIDTH / 8;
    custom->bltdmod = WIDTH / 8;
    custom->bltapt = bltapt;
    custom->bltcpt = dst;
    custom->bltdpt = src;
    custom->bltsize = bltsize;
  }
}

typedef struct {
  UWORD key;
  WORD index;
} ItemT;

static ItemT faceArray0[256];
static ItemT faceArray1[256];
static WORD faceCount;

static __regargs void CalculateFaceDepth(Object3D *object) {
  IndexListT **faces = object->mesh->face;
  WORD n = object->mesh->faces;
  Point3D *point = object->vertex;
  BYTE *faceFlags = object->faceFlags;
  WORD count = 0;
  WORD index = 0;

  WORD *item = (WORD *)faceArray0;

  while (--n >= 0) {
    IndexListT *face = *faces++;

    if (*faceFlags++) {
      WORD vs = face->count;
      WORD *vi = face->indices;
      LONG sum = 0;
      WORD z;

      while (--vs >= 0)
        sum += point[*vi++].z;

      z = div16(sum, face->count);

      *item++ = z + 32768;
      *item++ = index;
      count++;
    }
    index++;
  }

  faceCount = count;
}

static __regargs void SortFaces(Object3D *object) {
  static UBYTE bucket[16];

  ItemT *item0 = faceArray0;
  ItemT *item1 = faceArray1;
  WORD pass = 4;

  while (--pass >= 0) {
    {
      LONG *data = (LONG *)bucket;
      WORD n = 4;

      while (--n >= 0)
        *data++ = 0;
    }

    /* Count how many number has the same least significant digit. */
    {
      ItemT *item = item0;
      WORD n = faceCount;

      while (--n >= 0) {
        WORD digit = item->key & 15;
        bucket[digit]++;
        item++;
      }
    }

    /* Calculate bucket positions. */
    {
      BYTE *sum = bucket;
      WORD i = 0;
      WORD n = 16;

      while (--n >= 0) {
        WORD c = *sum;
        *sum++ = i;
        i += c;
      }
    }

    /* Move items to buckets. */
    {
      WORD *src = (WORD *)item0;
      WORD n = faceCount;

      while (--n >= 0) {
        WORD key = *src++;
        WORD digit = key & 15;
        WORD i = bucket[digit];
        WORD *dst = (WORD *)&item1[i];

        bucket[digit]++;

        *dst++ = key >> 4;
        *dst++ = *src++;
      }
    }

    swapr(item0, item1);
  }
}

static void DrawObject(Object3D *object, APTR *screen, APTR buffer) {
  Point3D *point = object->vertex;
  BYTE *faceFlags = object->faceFlags;
  IndexListT **faceEdges = object->mesh->faceEdge;
  IndexListT **faces = object->mesh->face;
  ItemT *item = faceArray0;
  WORD n = faceCount;

  custom->bltafwm = -1;
  custom->bltalwm = -1;

  while (--n >= 0) {
    LONG index = item->index;
    IndexListT *face = faces[index];
    IndexListT *faceEdge = faceEdges[index];
    BYTE color = faceFlags[index] >> 1;

    item++;

    {
      UWORD bltmod, bltsize;
      WORD bltstart, bltend;

      /* Estimate the size of rectangle that contains a face. */
      {
        WORD *i = face->indices;
        Point3D *p = &point[*i++];
        WORD minX = p->x;
        WORD minY = p->y;
        WORD maxX = minX; 
        WORD maxY = minY;
        WORD n = face->count - 2;

        do {
          p = &point[*i++];

          if (p->x < minX)
            minX = p->x;
          else if (p->x > maxX)
            maxX = p->x;

          if (p->y < minY)
            minY = p->y;
          else if (p->y > maxY)
            maxY = p->y;
        } while (--n != -1);

        /* Align to word boundary. */
        minX &= ~15;
        maxX += 16; /* to avoid case where a line is on right edge */
        maxX &= ~15;

        {
          WORD w = maxX - minX;
          WORD h = maxY - minY + 1;

          bltstart = (minX >> 3) + minY * WIDTH / 8;
          bltend = (maxX >> 3) + maxY * WIDTH / 8 - 2;
          bltsize = (h << 6) | (w >> 4);
          bltmod = (WIDTH / 8) - (w >> 3);
        }
      }

      /* Draw face. */
      {
        EdgeT *edges = object->mesh->edge;
        WORD m = faceEdge->count;
        WORD *i = faceEdge->indices;

        while (--m >= 0) {
          WORD *edge = (WORD *)&edges[*i++];

          WORD *p0 = (APTR)point + *edge++;
          WORD *p1 = (APTR)point + *edge++;

          WORD x0 = *p0++;
          WORD y0 = *p0++;
          WORD x1 = *p1++;
          WORD y1 = *p1++;

          if (y0 > y1) {
            swapr(x0, x1);
            swapr(y0, y1);
          }

          DrawLine(x0, y0, x1, y1);
        }
      }

      /* Fill face. */
      {
        APTR src = buffer + bltend;

        WaitBlitter();

        custom->bltcon0 = (SRCA | DEST) | A_TO_D;
        custom->bltcon1 = BLITREVERSE | FILL_XOR;
        custom->bltapt = src;
        custom->bltdpt = src;
        custom->bltamod = bltmod;
        custom->bltdmod = bltmod;
        custom->bltsize = bltsize;
      }

      /* Copy filled face to screen. */
      {
        APTR src = buffer + bltstart;
        BYTE mask = 1 << (DEPTH - 1);
        WORD n = DEPTH;

        while (--n >= 0) {
          APTR dst = screen[n] + bltstart;
          UWORD bltcon0;

          if (color & mask)
            bltcon0 = (SRCA | SRCB | DEST) | A_OR_B;
          else
            bltcon0 = (SRCA | SRCB | DEST) | (NABC | NABNC);

          WaitBlitter();

          custom->bltcon0 = bltcon0;
          custom->bltcon1 = 0;
          custom->bltapt = src;
          custom->bltbpt = dst;
          custom->bltdpt = dst;
          custom->bltamod = bltmod;
          custom->bltbmod = bltmod;
          custom->bltdmod = bltmod;
          custom->bltsize = bltsize;

          mask >>= 1;
        }
      }

      /* Clear working area. */
      {
        APTR data = buffer + bltstart;

        WaitBlitter();

        custom->bltcon0 = (DEST | A_TO_D);
        custom->bltcon1 = 0;
        custom->bltadat = 0;
        custom->bltdpt = data;
        custom->bltdmod = bltmod;
        custom->bltsize = bltsize;
      }
    }
  }
}

static void Render() {
  BitmapClear(screen0, DEPTH);

  {
    // LONG lines = ReadLineCounter();
    cube->rotate.x = cube->rotate.y = cube->rotate.z = frameCount * 8;
    UpdateObjectTransformation(cube);
    UpdateFaceVisibility(cube);
    UpdateVertexVisibility(cube);
    TransformVertices(cube);
    // Log("transform: %ld\n", ReadLineCounter() - lines);
  }

  {
    // LONG lines = ReadLineCounter();
    CalculateFaceDepth(cube);
    SortFaces(cube);
    // Log("sort: %ld\n", ReadLineCounter() - lines);
  }

  WaitBlitter();

  {
    // LONG lines = ReadLineCounter();
    DrawObject(cube, screen0->planes, buffer->planes[0]);
    // Log("draw: %ld\n", ReadLineCounter() - lines);
  }

  WaitVBlank();

  {
    WORD n = DEPTH;

    while (--n >= 0)
      CopInsSet32(bplptr[n], screen0->planes[n]);
  }

  swapr(screen0, screen1);
}

EffectT Effect = { Load, UnLoad, Init, Kill, Render };
