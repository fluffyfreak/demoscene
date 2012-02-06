#include <math.h>

#include "p61/p61.h"

#include "gfx/blit.h"
#include "gfx/line.h"
#include "gfx/palette.h"
#include "gfx/transformations.h"
#include "std/resource.h"

#include "system/c2p.h"
#include "system/debug.h"
#include "system/display.h"
#include "system/memory.h"
#include "system/vblank.h"

#include "frame_tools.h"
#include "distortion.h"

const int WIDTH = 320;
const int HEIGHT = 256;
const int DEPTH = 8;

static CanvasT *Canvas;
static DBufRasterT *Raster;
static DistortionMapT *TunnelMap;
static PixBufT *Texture;
static PixBufT *CreditsCode;
static PixBufT *CreditsMusic;
static PixBufT *Anniversary;

static PointT *Cross;
static PointT CrossToDraw[12];

/*
 * Set up display function.
 */
struct ViewPort *SetupDisplay() {
  if ((Raster = NewDBufRaster(WIDTH, HEIGHT, DEPTH))) {
    ConfigureViewPort(Raster->ViewPort);

    return Raster->ViewPort;
  }

  return NULL;
}

/*
 * Tear down display function.
 */
void TearDownDisplay() {
  DeleteDBufRaster(Raster);
}

/*
 * Set up effect function.
 */
void SetupEffect() {
  Texture = GetResource("txt_img");
  CreditsCode = GetResource("code_img");
  CreditsMusic = GetResource("music_img");
  Anniversary = GetResource("anniversary_img");
  TunnelMap = GetResource("tunnel_map");
  Cross = GetResource("cross");

  Canvas = NewCanvas(WIDTH, HEIGHT);

  {
    PaletteT *texturePal = GetResource("txt_pal");
    PaletteT *codePal = GetResource("code_pal");
    PaletteT *musicPal = GetResource("music_pal");
    PaletteT *anniversaryPal = GetResource("anniversary_pal");

    LinkPalettes(4, texturePal, codePal, anniversaryPal, musicPal);

    LoadPalette(Raster->ViewPort, texturePal);
    SetColor(Raster->ViewPort, 255, 255, 255, 255);

    PixBufRemap(CreditsCode, codePal);
    PixBufRemap(CreditsMusic, musicPal);
    PixBufRemap(Anniversary, anniversaryPal);
  }

  TS_Init();

  P61_Init(GetResource("module"), NULL, NULL);
  P61_ControlBlock.Play = 1;
}

/*
 * Tear down effect function.
 */
void TearDownEffect() {
  P61_End();
  TS_End();

  UnlinkPalettes(GetResource("txt_pal"));
  DeleteCanvas(Canvas);
}

/*
 * Rendering functions.
 */
void RenderTunnel(int frameNumber, DBufRasterT *raster) {
  RenderDistortion(Canvas, TunnelMap, Texture, 0, frameNumber);

  float s = sin(frameNumber * 3.14159265f / 45.0f);
  float c = cos(frameNumber * 3.14159265f / 90.0f);

  TS_Reset();
  TS_PushTranslation2D(-1.5f, -1.5f);
  TS_PushScaling2D(20.0f + 10.0f * s, 20.0f + 10.0f * s);
  TS_Compose2D();
  TS_PushRotation2D((float)(frameNumber * -2));
  TS_Compose2D();
  TS_PushTranslation2D((float)(WIDTH/2) + c * (WIDTH/4), (float)(HEIGHT/2 + 40));
  TS_Compose2D();

  M2D_Transform(CrossToDraw, Cross, 12, TS_GetMatrix2D(1));

  DrawPolyLine(Canvas, CrossToDraw, 12, TRUE);

  PixBufBlitTransparent(Canvas->pixbuf, 20, 200, CreditsCode);
  PixBufBlitTransparent(Canvas->pixbuf, 100, 10, Anniversary);
  PixBufBlitTransparent(Canvas->pixbuf, 230, 200, CreditsMusic);
}

void RenderChunky(int frameNumber, DBufRasterT *raster) {
  c2p1x1_8_c5_bm(GetCanvasPixelData(Canvas), raster->BitMap, WIDTH, HEIGHT, 0, 0);
}

/*
 * Main loop.
 */
void MainLoop() {
  SetVBlankCounter(0);

  while (GetVBlankCounter() < 500) {
    WaitForSafeToWrite(Raster);

    int frameNumber = GetVBlankCounter();

    RenderTunnel(frameNumber, Raster);
    RenderChunky(frameNumber, Raster);
    RenderFrameNumber(frameNumber, Raster);

    WaitForSafeToSwap(Raster);
    DBufRasterSwap(Raster);
  }
}
