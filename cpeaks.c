/*
 * cpeaks - a Twin Peaks "Red Room" themed Matrix-rain for your terminal.
 *
 * Spiritual fork of cmatrix (Chris Allegretta / Abishek V Ashok, GPL-3.0).
 * Red rain falls from the top and settles into an ASCII glyph-mosaic of the
 * Red Room - the Venus de Milo statue framed against drifting red curtains
 * and the black/white chevron floor. Once the image forms, the curtains
 * drift slowly as if in a warm breeze. Press any key to leave.
 *
 * The image is embedded in the binary and quantized to a bespoke 240-colour
 * palette derived from the photo itself, so the colour scheme stays faithful.
 *
 * License: GPL-3.0 (inherits cmatrix's license).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <locale.h>

#include "vendor/stb_image.h"
#include "vendor/stb_image_write.h"
#include "vendor/font8x8_basic.h"  /* font8x8_basic[128][8], public domain */
#include "redroom_png.h"   /* redroom_embed_png[], redroom_embed_png_len */

#include <curses.h>

/* ----------------------------------------------------------------------- */
/* Tunables                                                                 */
/* ----------------------------------------------------------------------- */

#define PAL_BASE     16          /* first ncurses colour slot we own        */
#define N_RAIN       6           /* synthetic warm rain-ramp colours        */
#define N_IMG        (240 - N_RAIN)
#define N_PAL        240         /* total palette entries (img + rain)      */

/* Statue focal point + bounding box in normalized source coords [0,1].
 * The statue is the hero: framing is anchored here so it stays centred and
 * fully visible regardless of terminal aspect ratio. */
#define FOCAL_X      0.515
#define FOCAL_Y      0.400
#define STATUE_X0    0.40
#define STATUE_X1    0.64
#define STATUE_Y0    0.14
#define STATUE_Y1    0.66
#define STATUE_FRAC  0.78        /* statue height as fraction of viewport    */

#define FLOOR_LINE   0.605       /* src y below this is the chevron floor     */

static double cell_aspect = 2.0; /* glyph height/width ratio (tall cells)    */

/* Matrix glyph set for the falling rain (random, no wide chars). */
static const char *GLYPHS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"
    "@#$%&*+=-<>{}[]()/\\|?!:;";
static int N_GLYPHS;

/* Tonal ramp (light -> dense) for the SETTLED image: each cell picks a glyph
 * by its brightness, so the picture is carried by glyph density as well as
 * colour. Dark cells become spaces (black negative space); bright cells become
 * dense bold glyphs. This is what makes the statue read in real terminal text. */
static const char *RAMP =
    " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";
static int RAMP_LEN;

/* Region tags (drift only the curtains). */
enum { R_CURTAIN = 0, R_STATUE = 1, R_FLOOR = 2, R_DARK = 3 };

/* ----------------------------------------------------------------------- */
/* Globals                                                                  */
/* ----------------------------------------------------------------------- */

static unsigned char *img = NULL;   /* source RGB, iw*ih*3 */
static int iw = 0, ih = 0;

static unsigned char pal[N_PAL][3]; /* quantized palette (true RGB)         */

/* Per-cell settled target, sized to the terminal grid. */
static int   cols = 0, rows = 0;
static int   *t_pal   = NULL;       /* settled palette index                */
static int   *t_glyph = NULL;       /* settled glyph index                  */
static unsigned char *t_rgb = NULL; /* settled true RGB (3/cell) for drift   */
static char  *t_region = NULL;
static double *t_sx = NULL;         /* normalized src x of each cell         */

#define CELL(y,x) ((y)*cols + (x))

/* ----------------------------------------------------------------------- */
/* Image load + median-cut quantization                                     */
/* ----------------------------------------------------------------------- */

static void load_image(void) {
    int n;
    img = stbi_load_from_memory(redroom_embed_png, (int)redroom_embed_png_len,
                                &iw, &ih, &n, 3);
    if (!img) {
        fprintf(stderr, "cpeaks: failed to decode embedded image\n");
        exit(1);
    }
}

/* Region-aware relighting so the statue is the luminance hero:
 *  - STATUE: bright marble, strongly desaturated (pops against the red).
 *  - FLOOR : whiten the light chevron stripes, keep dark stripes dark.
 *  - CURTAIN: deep, rich red that recedes (modest lift only) so it doesn't
 *    out-shine the statue.
 * Applied once to the source so live render and snapshot stay identical. */
static double e_contrast = 1.28;     /* global contrast (>1 = more)           */

/* statue visual box (tight) + how "not red" a pixel must be to count as marble */
#define ST_X0 0.40
#define ST_X1 0.64
#define ST_Y0 0.13
#define ST_Y1 0.62
#define ST_REDNESS 64.0              /* R-max(G,B) below this -> marble, not drape */

static double smoothstep(double a, double b, double x) {
    if (b <= a) return x < a ? 0.0 : 1.0;
    double t = (x - a) / (b - a);
    if (t < 0) t = 0; if (t > 1) t = 1;
    return t * t * (3.0 - 2.0 * t);
}

static void enhance_image(void) {
    for (int y = 0; y < ih; y++) {
        double sy = (double)y / ih;
        for (int x = 0; x < iw; x++) {
            double sx = (double)x / iw;
            long i = (long)y * iw + x;
            double c[3];
            for (int k = 0; k < 3; k++)
                c[k] = (img[i*3+k] - 128.0) * e_contrast + 128.0;   /* contrast */

            double gray = 0.299*c[0] + 0.587*c[1] + 0.114*c[2];
            double mx = c[0]>c[1]?(c[0]>c[2]?c[0]:c[2]):(c[1]>c[2]?c[1]:c[2]);
            double mn = c[0]<c[1]?(c[0]<c[2]?c[0]:c[2]):(c[1]<c[2]?c[1]:c[2]);
            double chroma = mx - mn;
            double redness = c[0] - (c[1] > c[2] ? c[1] : c[2]);

            double L, S;   /* target luminance, saturation multiplier */

            int in_statue = (sx > ST_X0 && sx < ST_X1 && sy > ST_Y0 && sy < ST_Y1
                             && redness < ST_REDNESS);
            int in_floor  = (sy > FLOOR_LINE);

            if (in_statue) {
                /* bright marble: lift luminance, near-neutral */
                L = gray * 1.10 + 58.0;
                S = 0.24;
            } else if (in_floor) {
                double light = smoothstep(120.0, 200.0, gray); /* light stripe? */
                double t = smoothstep(30.0, 120.0, chroma);
                S = (0.42*(1.0-t) + 1.18*t) * (1.0 - 0.75*light);
                L = gray + 6.0 + 26.0*light;                   /* whiten lights */
            } else {
                /* curtain: deep rich red, only a small lift so it recedes */
                double t = smoothstep(30.0, 120.0, chroma);
                S = 0.45*(1.0-t) + 1.20*t;
                L = gray * 0.94 + 10.0;
            }

            for (int k = 0; k < 3; k++) {
                double v = L + (c[k] - gray) * S;
                if (v < 0) v = 0; if (v > 255) v = 255;
                img[i*3+k] = (unsigned char)(v + 0.5);
            }
        }
    }
}

/* median cut over a (subsampled) pixel array -> N_IMG colours */
typedef struct { unsigned char r, g, b; } RGB;
static int mc_channel;

static int mc_cmp(const void *a, const void *b) {
    const RGB *p = a, *q = b;
    int pa = (mc_channel == 0) ? p->r : (mc_channel == 1) ? p->g : p->b;
    int qa = (mc_channel == 0) ? q->r : (mc_channel == 1) ? q->g : q->b;
    return pa - qa;
}

typedef struct { int lo, hi; } Box;

static void box_ranges(RGB *px, Box b, int *rr, int *rg, int *rb, int *longest) {
    int rmin=255,rmax=0,gmin=255,gmax=0,bmin=255,bmax=0;
    for (int i = b.lo; i < b.hi; i++) {
        RGB p = px[i];
        if (p.r<rmin)rmin=p.r; if (p.r>rmax)rmax=p.r;
        if (p.g<gmin)gmin=p.g; if (p.g>gmax)gmax=p.g;
        if (p.b<bmin)bmin=p.b; if (p.b>bmax)bmax=p.b;
    }
    *rr = rmax-rmin; *rg = gmax-gmin; *rb = bmax-bmin;
    *longest = (*rr>=*rg && *rr>=*rb) ? 0 : (*rg>=*rb ? 1 : 2);
}

static void build_palette(void) {
    /* subsample the image into a pixel pool */
    int step = 1;
    long total = (long)iw * ih;
    while (total / (step*step) > 120000) step++;
    int np = 0;
    int cap = (iw/step + 1) * (ih/step + 1);
    RGB *px = malloc(sizeof(RGB) * cap);
    for (int y = 0; y < ih; y += step)
        for (int x = 0; x < iw; x += step) {
            unsigned char *p = img + (y*iw + x)*3;
            px[np].r = p[0]; px[np].g = p[1]; px[np].b = p[2]; np++;
        }

    Box *boxes = malloc(sizeof(Box) * N_IMG);
    int nb = 1;
    boxes[0].lo = 0; boxes[0].hi = np;

    while (nb < N_IMG) {
        /* pick the box with the largest single-channel range */
        int best = -1, bestrange = -1, bestchan = 0;
        for (int i = 0; i < nb; i++) {
            if (boxes[i].hi - boxes[i].lo <= 1) continue;
            int rr,rg,rb,lng;
            box_ranges(px, boxes[i], &rr,&rg,&rb,&lng);
            int rng = (lng==0)?rr:(lng==1)?rg:rb;
            if (rng > bestrange) { bestrange = rng; best = i; bestchan = lng; }
        }
        if (best < 0) break;               /* nothing splittable */
        Box b = boxes[best];
        mc_channel = bestchan;
        qsort(px + b.lo, b.hi - b.lo, sizeof(RGB), mc_cmp);
        int mid = (b.lo + b.hi) / 2;
        boxes[best].lo = b.lo; boxes[best].hi = mid;
        boxes[nb].lo  = mid;   boxes[nb].hi  = b.hi;
        nb++;
    }

    for (int i = 0; i < nb; i++) {
        long sr=0,sg=0,sb=0; int c = boxes[i].hi - boxes[i].lo;
        if (c <= 0) { pal[i][0]=pal[i][1]=pal[i][2]=0; continue; }
        for (int j = boxes[i].lo; j < boxes[i].hi; j++) {
            sr += px[j].r; sg += px[j].g; sb += px[j].b;
        }
        pal[i][0] = sr/c; pal[i][1] = sg/c; pal[i][2] = sb/c;
    }
    for (int i = nb; i < N_IMG; i++) { pal[i][0]=pal[i][1]=pal[i][2]=0; }

    /* synthetic warm rain ramp occupies the final N_RAIN slots */
    unsigned char rain[N_RAIN][3] = {
        {255,232,226}, {255,150,135}, {236, 52, 48},
        {176, 22, 26}, {110, 12, 18}, { 56,  6, 10},
    };
    for (int i = 0; i < N_RAIN; i++)
        memcpy(pal[N_IMG + i], rain[i], 3);

    free(px); free(boxes);
}

/* nearest palette index over the image colours (0..N_IMG-1) */
static int nearest_img(int r, int g, int b) {
    int best = 0; long bd = 1L<<60;
    for (int i = 0; i < N_IMG; i++) {
        int dr=r-pal[i][0], dg=g-pal[i][1], db=b-pal[i][2];
        long d = (long)dr*dr + (long)dg*dg + (long)db*db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* ----------------------------------------------------------------------- */
/* Framing: statue-anchored "cover" sampling                                */
/* ----------------------------------------------------------------------- */

static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* average source RGB over a cell's footprint (edge-clamped) */
static void sample_cell(double cx_src, double cy_src, double sx, double sy,
                        int *r, int *g, int *b) {
    int x0 = (int)floor(cx_src - sx*0.5), x1 = (int)ceil(cx_src + sx*0.5);
    int y0 = (int)floor(cy_src - sy*0.5), y1 = (int)ceil(cy_src + sy*0.5);
    /* cap the number of samples so huge cells stay cheap */
    int xs = (x1-x0)>8 ? (x1-x0)/8 : 1;
    int ys = (y1-y0)>8 ? (y1-y0)/8 : 1;
    long sr=0,sg=0,sb=0; int n=0;
    for (int y = y0; y <= y1; y += ys) {
        int yy = clampi(y, 0, ih-1);
        for (int x = x0; x <= x1; x += xs) {
            int xx = clampi(x, 0, iw-1);
            unsigned char *p = img + (yy*iw + xx)*3;
            sr += p[0]; sg += p[1]; sb += p[2]; n++;
        }
    }
    if (n == 0) n = 1;
    *r = sr/n; *g = sg/n; *b = sb/n;
}

static char classify(int r, int g, int b, double sy_norm, double sx_norm) {
    int lum = (299*r + 587*g + 114*b) / 1000;
    int redness = r - (g + b)/2;
    if (sy_norm > FLOOR_LINE) return R_FLOOR;
    if (sx_norm > STATUE_X0 - 0.04 && sx_norm < STATUE_X1 + 0.04 &&
        sy_norm > STATUE_Y0 && sy_norm < STATUE_Y1 && redness < 55)
        return (lum < 24) ? R_DARK : R_STATUE;
    if (redness < 22 && lum < 28) return R_DARK;
    return R_CURTAIN;
}

static void free_grid(void) {
    free(t_pal); free(t_glyph); free(t_rgb); free(t_region); free(t_sx);
    t_pal=NULL; t_glyph=NULL; t_rgb=NULL; t_region=NULL; t_sx=NULL;
}

/* compute the settled target mosaic for a cols x rows grid */
static void build_target(int c, int r_) {
    cols = c; rows = r_;
    free_grid();
    t_pal    = malloc(sizeof(int) * cols * rows);
    t_glyph  = malloc(sizeof(int) * cols * rows);
    t_rgb    = malloc(3 * cols * rows);
    t_region = malloc(cols * rows);
    t_sx     = malloc(sizeof(double) * cols * rows);

    /* source pixels per cell (horizontal); statue sized to STATUE_FRAC */
    double statue_h = (STATUE_Y1 - STATUE_Y0) * ih;
    double s = statue_h / (rows * cell_aspect * STATUE_FRAC);
    if (s < 0.25) s = 0.25;

    double cx0 = FOCAL_X * iw;
    double cy0 = FOCAL_Y * ih;

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            double cx_src = cx0 + (x - (cols-1)/2.0) * s;
            double cy_src = cy0 + (y - (rows-1)/2.0) * s * cell_aspect;
            int r,g,b;
            sample_cell(cx_src, cy_src, s, s*cell_aspect, &r,&g,&b);
            double sx_norm = cx_src / iw;
            double sy_norm = cy_src / ih;
            int pi = nearest_img(r,g,b);
            int idx = CELL(y,x);
            t_pal[idx]   = pi;
            /* glyph by brightness: tonal ramp index (dark->space, bright->dense) */
            int lum = (299*r + 587*g + 114*b) / 1000;
            double f = lum / 255.0;
            f = pow(f, 1.15);                       /* deepen shadows a touch     */
            int gi = (int)(f * (RAMP_LEN - 1) + 0.5);
            if (gi < 0) gi = 0; if (gi >= RAMP_LEN) gi = RAMP_LEN - 1;
            t_glyph[idx] = gi;
            t_rgb[idx*3+0] = pal[pi][0];
            t_rgb[idx*3+1] = pal[pi][1];
            t_rgb[idx*3+2] = pal[pi][2];
            t_region[idx]  = classify(r,g,b, sy_norm, sx_norm);
            t_sx[idx]      = sx_norm;
        }
    }
}

/* ----------------------------------------------------------------------- */
/* Snapshot: render the settled mosaic to a PNG for verification            */
/* ----------------------------------------------------------------------- */

/* blit one 8x8 glyph, scaled to an 8x16 cell, in colour on black */
static void blit_glyph(unsigned char *out, int W, int ox, int oy,
                       char ch, unsigned char *rgb) {
    if ((unsigned char)ch >= 128) ch = '?';
    const char *bm = font8x8_basic[(int)ch];
    for (int fy = 0; fy < 8; fy++) {
        unsigned char bits = (unsigned char)bm[fy];
        for (int fx = 0; fx < 8; fx++) {
            if (!(bits & (1u << fx))) continue;
            int px = ox + fx;
            for (int d = 0; d < 2; d++) {       /* 8 rows -> 16 (cell is 2:1) */
                int py = oy + fy*2 + d;
                unsigned char *o = out + ((size_t)py*W + px)*3;
                o[0]=rgb[0]; o[1]=rgb[1]; o[2]=rgb[2];
            }
        }
    }
}

/* render the settled mosaic as real colour-on-black glyphs, like the terminal */
static int snapshot(const char *path, int c, int r_) {
    N_GLYPHS = strlen(GLYPHS); RAMP_LEN = strlen(RAMP);
    build_target(c, r_);
    int cw = 8, ch = 16;                 /* px per cell (2:1)               */
    int W = cols*cw, H = rows*ch;
    unsigned char *out = calloc((size_t)W*H*3, 1);   /* black background     */
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            int idx = CELL(y,x);
            blit_glyph(out, W, x*cw, y*ch, RAMP[t_glyph[idx]], &t_rgb[idx*3]);
        }
    int ok = stbi_write_png(path, W, H, 3, out, W*3);
    free(out);
    return ok;
}

/* region-debug snapshot: paints regions as flat colours */
static int snapshot_regions(const char *path, int c, int r_) {
    N_GLYPHS = strlen(GLYPHS); RAMP_LEN = strlen(RAMP);
    build_target(c, r_);
    int cw = 8, ch = 16;
    int W = cols*cw, H = rows*ch;
    unsigned char *out = calloc((size_t)W*H*3, 1);
    unsigned char rc[4][3] = {{220,30,30},{240,240,210},{60,140,200},{20,20,20}};
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            unsigned char *col = rc[(int)t_region[CELL(y,x)]];
            for (int py = y*ch; py < (y+1)*ch; py++)
                for (int px = x*cw; px < (x+1)*cw; px++) {
                    unsigned char *o = out + ((size_t)py*W + px)*3;
                    o[0]=col[0]; o[1]=col[1]; o[2]=col[2];
                }
        }
    int ok = stbi_write_png(path, W, H, 3, out, W*3);
    free(out);
    return ok;
}

/* ----------------------------------------------------------------------- */
/* ncurses colour setup                                                     */
/* ----------------------------------------------------------------------- */

static int xterm256_of(int r, int g, int b) {
    /* nearest xterm-256 (6x6x6 cube + grayscale) for the no-change fallback */
    int qr = (r<48)?0:(r<115)?1:(r-35)/40;
    int qg = (g<48)?0:(g<115)?1:(g-35)/40;
    int qb = (b<48)?0:(b<115)?1:(b-35)/40;
    if (qr>5)qr=5; if(qg>5)qg=5; if(qb>5)qb=5;
    int cube = 16 + 36*qr + 6*qg + qb;
    int gray = (r+g+b)/3;
    int gidx = 232 + clampi((gray-8)/10, 0, 23);
    /* pick whichever is closer */
    int cv[3] = { qr?qr*40+55:0, qg?qg*40+55:0, qb?qb*40+55:0 };
    int gv = 8 + 10*clampi((gray-8)/10,0,23);
    long dc = (long)(r-cv[0])*(r-cv[0])+(g-cv[1])*(g-cv[1])+(b-cv[2])*(b-cv[2]);
    long dg = (long)(r-gv)*(r-gv)+(g-gv)*(g-gv)+(b-gv)*(b-gv);
    return (dg < dc) ? gidx : cube;
}

static int use_truecolor_pal = 0;

static void init_colors(void) {
    start_color();
    if (can_change_color() && COLORS >= 256) {
        use_truecolor_pal = 1;
        for (int i = 0; i < N_PAL; i++) {
            init_color(PAL_BASE + i,
                       pal[i][0]*1000/255, pal[i][1]*1000/255, pal[i][2]*1000/255);
            init_pair(i + 1, PAL_BASE + i, COLOR_BLACK);
        }
    } else {
        use_truecolor_pal = 0;
        for (int i = 0; i < N_PAL; i++)
            init_pair(i + 1, xterm256_of(pal[i][0],pal[i][1],pal[i][2]), COLOR_BLACK);
    }
}

static inline void draw_chr(int y, int x, char ch, int palidx, int bold) {
    attr_t a = COLOR_PAIR(palidx + 1);
    if (bold) a |= A_BOLD;
    attrset(a);
    mvaddch(y, x, (chtype)(unsigned char)ch);
}

/* draw a settled image cell: tonal glyph, with bold on the brightest cells */
static inline void draw_settled(int y, int x, int idx) {
    int gi = t_glyph[idx];
    int bold = gi > (int)(RAMP_LEN * 0.70);
    draw_chr(y, x, RAMP[gi], t_pal[idx], bold);
}

/* nearest palette index across the FULL palette (img + rain) for drift/glow */
static int nearest_full(int r, int g, int b) {
    int best = 0; long bd = 1L<<60;
    for (int i = 0; i < N_PAL; i++) {
        int dr=r-pal[i][0], dg=g-pal[i][1], db=b-pal[i][2];
        long d = (long)dr*dr + (long)dg*dg + (long)db*db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* ----------------------------------------------------------------------- */
/* Animation                                                                */
/* ----------------------------------------------------------------------- */

static volatile sig_atomic_t resized = 0;
static void on_winch(int s){ (void)s; resized = 1; }

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec*1e-9;
}

static void msleep(int ms){ struct timespec t={ms/1000,(ms%1000)*1000000L}; nanosleep(&t,NULL); }

/* per-column fall state */
static double *head = NULL;   /* current front row (grows past rows)         */
static double *spd  = NULL;
static int    *taillen = NULL;

#define GLOW 7

static void fall_init(void) {
    free(head); free(spd); free(taillen);
    head = malloc(sizeof(double)*cols);
    spd  = malloc(sizeof(double)*cols);
    taillen = malloc(sizeof(int)*cols);
    for (int x = 0; x < cols; x++) {
        head[x] = -(rand() % (rows/3 + GLOW));    /* tight staggered start    */
        spd[x]  = 1.4 + (rand()%100)/100.0*1.2;   /* fast async fall (~2s)    */
        taillen[x] = GLOW;
    }
}

/* render one fall frame; returns 1 when every column has fully settled */
static int fall_frame(int speed_div) {
    int settled = 1;
    for (int x = 0; x < cols; x++) {
        double hy = head[x];
        for (int y = 0; y < rows; y++) {
            int idx = CELL(y,x);
            if (y <= hy - GLOW) {
                /* fully cooled -> settled image */
                draw_settled(y, x, idx);
            } else if (y <= hy) {
                /* glow tail: hot near the head, cooling into the image */
                double d = hy - y;                 /* 0 at head .. GLOW up    */
                double f = d / (double)GLOW;        /* 0 hot .. 1 cool         */
                int hr=236,hg=52,hb=48;             /* hot red reference       */
                if (d < 0.8) { hr=255; hg=232; hb=226; } /* bright head        */
                int tr = t_rgb[idx*3+0], tg = t_rgb[idx*3+1], tb = t_rgb[idx*3+2];
                int r = (int)(hr*(1-f) + tr*f);
                int g = (int)(hg*(1-f) + tg*f);
                int b = (int)(hb*(1-f) + tb*f);
                int pi = nearest_full(r,g,b);
                /* random matrix glyph near the head, settling to the tonal glyph */
                char ch = (d < 1.5) ? GLYPHS[rand()%N_GLYPHS] : RAMP[t_glyph[idx]];
                draw_chr(y, x, ch, pi, d < 2.0);
            } else {
                /* not yet reached */
                mvaddch(y, x, ' ');
            }
        }
        if (hy - GLOW < rows - 1) settled = 0;
        head[x] += spd[x] / (double)speed_div;
    }
    return settled;
}

/* curtain drift: a slow horizontal luminance wave (warm breeze) */
static void drift_frame(double t) {
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            int idx = CELL(y,x);
            if (t_region[idx] != R_CURTAIN) {
                draw_settled(y, x, idx);
                continue;
            }
            double sx = t_sx[idx];
            /* two travelling waves of different speed/length for organic folds */
            double w = 0.55*sin(sx*9.0  - t*0.82)
                     + 0.45*sin(sx*17.0 - t*1.22 + y*0.05);
            double amp = 0.26;
            int r = t_rgb[idx*3+0], g = t_rgb[idx*3+1], b = t_rgb[idx*3+2];
            double m = 1.0 + amp*w;
            int rr = clampi((int)(r*m), 0, 255);
            int gg = clampi((int)(g*m), 0, 255);
            int bb = clampi((int)(b*m), 0, 255);
            int pi = nearest_full(rr,gg,bb);
            int gi = t_glyph[idx];
            draw_chr(y, x, RAMP[gi], pi, gi > (int)(RAMP_LEN*0.70));
        }
    }
}

static void run(int speed_div, int do_drift) {
    initscr();
    if (!has_colors()) { endwin(); fprintf(stderr,"cpeaks: terminal has no colour support\n"); exit(1); }
    init_colors();
    noecho(); curs_set(0); cbreak(); nodelay(stdscr, TRUE); keypad(stdscr, TRUE);
    signal(SIGWINCH, on_winch);

    int frame_ms = 33;

rebuild:
    getmaxyx(stdscr, rows, cols);
    build_target(cols, rows);
    bkgd(COLOR_PAIR(0));
    erase();
    fall_init();

    /* fall phase */
    for (;;) {
        if (resized) { resized = 0; endwin(); refresh(); goto rebuild; }
        int ch = getch();
        if (ch != ERR) { endwin(); return; }
        int done = fall_frame(speed_div);
        refresh();
        if (done) break;
        msleep(frame_ms);
    }

    /* drift phase (forever, until a key) */
    double t0 = now_sec();
    for (;;) {
        if (resized) { resized = 0; endwin(); refresh(); goto rebuild; }
        int ch = getch();
        if (ch != ERR) { endwin(); return; }
        if (do_drift) drift_frame(now_sec() - t0);
        refresh();
        msleep(do_drift ? 55 : 120);
    }
}

/* ----------------------------------------------------------------------- */
/* main                                                                     */
/* ----------------------------------------------------------------------- */

static void usage(void) {
    printf(
"cpeaks - Twin Peaks Red Room Matrix rain\n\n"
"Usage: cpeaks [options]\n"
"  -u N           update delay / speed divisor (1-10, default 1; higher=slower)\n"
"  -a F           cell aspect ratio (height/width, default 2.0)\n"
"  -n             no curtain drift (settle and hold)\n"
"  --snapshot F [W H]   render the settled image to PNG F (default grid 100x46)\n"
"  --regions F [W H]    render the region map to PNG F (debug)\n"
"  -h             this help\n"
"  -V             version\n\n"
"Press any key while running to exit.\n");
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    N_GLYPHS = strlen(GLYPHS);
    RAMP_LEN = strlen(RAMP);

    int speed_div = 1, do_drift = 1;
    const char *snap = NULL, *regf = NULL;
    int snapW = 100, snapH = 46;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (!strcmp(argv[i], "-V") || !strcmp(argv[i], "--version")) {
            printf("cpeaks 0.1.0\n"); return 0; }
        else if (!strcmp(argv[i], "-n")) do_drift = 0;
        else if (!strcmp(argv[i], "-u") && i+1 < argc) {
            speed_div = atoi(argv[++i]); if (speed_div < 1) speed_div = 1; if (speed_div>10) speed_div=10; }
        else if (!strcmp(argv[i], "-a") && i+1 < argc) {
            cell_aspect = atof(argv[++i]); if (cell_aspect < 0.5) cell_aspect = 0.5; }
        else if (!strcmp(argv[i], "--snapshot") && i+1 < argc) {
            snap = argv[++i];
            if (i+2 < argc && argv[i+1][0] != '-') { snapW = atoi(argv[++i]); snapH = atoi(argv[++i]); } }
        else if (!strcmp(argv[i], "--regions") && i+1 < argc) {
            regf = argv[++i];
            if (i+2 < argc && argv[i+1][0] != '-') { snapW = atoi(argv[++i]); snapH = atoi(argv[++i]); } }
        else { fprintf(stderr, "cpeaks: unknown option '%s'\n", argv[i]); return 2; }
    }

    load_image();
    enhance_image();
    build_palette();

    if (snap) {
        if (!snapshot(snap, snapW, snapH)) { fprintf(stderr,"cpeaks: write failed\n"); return 1; }
        printf("wrote %s (%dx%d cells)\n", snap, snapW, snapH); return 0;
    }
    if (regf) {
        if (!snapshot_regions(regf, snapW, snapH)) { fprintf(stderr,"cpeaks: write failed\n"); return 1; }
        printf("wrote %s (%dx%d cells)\n", regf, snapW, snapH); return 0;
    }

    run(speed_div, do_drift);
    return 0;
}
