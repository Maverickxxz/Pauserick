/* Generates src/pauserick.ico: a teal circle with a white "pause" symbol.
 * With "paused", the bars are red instead (src/pauserick_paused.ico, shown in the
 * tray while a game or listed app holds the breaks).
 * Build tool only; run by build.bat. Usage: mkicon <output.ico> [paused]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const int SIZES[] = { 16, 20, 24, 32, 40, 48, 64 };
#define NSIZES (int)(sizeof(SIZES) / sizeof(SIZES[0]))
#define SS 8 /* supersampling per axis */

static int in_round_rect(double x, double y, double x0, double y0, double x1, double y1, double r)
{
    if (x < x0 || x > x1 || y < y0 || y > y1) return 0;
    if (r <= 0) return 1;
    double cx = x < x0 + r ? x0 + r : (x > x1 - r ? x1 - r : x);
    double cy = y < y0 + r ? y0 + r : (y > y1 - r ? y1 - r : y);
    double dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: mkicon out.ico [paused]\n"); return 1; }
    /* bar colour: white, or red for the "paused for a process" variant */
    int paused = argc > 2 && !strcmp(argv[2], "paused");
    const double barR = 255, barG = paused ? 0x45 : 255, barB = paused ? 0x45 : 255;
    FILE *f = fopen(argv[1], "wb");
    if (!f) { perror(argv[1]); return 1; }

    unsigned imgSize[NSIZES], offset = 6 + 16 * NSIZES;
    for (int i = 0; i < NSIZES; i++) {
        int s = SIZES[i], maskRow = ((s + 31) / 32) * 4;
        imgSize[i] = 40 + s * s * 4 + maskRow * s;
    }
    put16(f, 0); put16(f, 1); put16(f, NSIZES);
    for (int i = 0; i < NSIZES; i++) {
        fputc(SIZES[i], f); fputc(SIZES[i], f); fputc(0, f); fputc(0, f);
        put16(f, 1); put16(f, 32); put32(f, imgSize[i]); put32(f, offset);
        offset += imgSize[i];
    }

    for (int i = 0; i < NSIZES; i++) {
        int s = SIZES[i], maskRow = ((s + 31) / 32) * 4;
        /* pixel-snapped pause bars */
        int barW = (int)lround(s * 0.16), gap = (int)lround(s * 0.14);
        if ((s - (2 * barW + gap)) & 1) gap++;
        int barH = 2 * (int)lround(s * 0.23);
        double bx0 = (s - (2 * barW + gap)) / 2.0, by0 = (s - barH) / 2.0;
        double rr = s >= 32 ? barW * 0.3 : 0.0;
        double c = s / 2.0, rad = s / 2.0 - s * 0.02;

        unsigned char *px = calloc((size_t)s * s, 4);
        unsigned char *mask = calloc((size_t)maskRow * s, 1);
        for (int y = 0; y < s; y++) {
            for (int x = 0; x < s; x++) {
                int inCircle = 0, inBar = 0;
                for (int sy = 0; sy < SS; sy++) {
                    for (int sx = 0; sx < SS; sx++) {
                        double fx = x + (sx + 0.5) / SS, fy = y + (sy + 0.5) / SS;
                        double dx = fx - c, dy = fy - c;
                        if (dx * dx + dy * dy <= rad * rad) {
                            inCircle++;
                            if (in_round_rect(fx, fy, bx0, by0, bx0 + barW, by0 + barH, rr) ||
                                in_round_rect(fx, fy, bx0 + barW + gap, by0, bx0 + 2 * barW + gap, by0 + barH, rr))
                                inBar++;
                        }
                    }
                }
                double a = inCircle / (double)(SS * SS);
                double w = inCircle ? inBar / (double)inCircle : 0.0; /* bar fraction inside circle */
                /* teal #0F766E -> bar colour */
                double r = 0x0F + (barR - 0x0F) * w, g = 0x76 + (barG - 0x76) * w, b = 0x6E + (barB - 0x6E) * w;
                int row = s - 1 - y; /* bottom-up */
                unsigned char *p = px + ((size_t)row * s + x) * 4;
                p[0] = (unsigned char)lround(b); p[1] = (unsigned char)lround(g);
                p[2] = (unsigned char)lround(r); p[3] = (unsigned char)lround(a * 255);
                if (p[3] == 0) mask[row * maskRow + x / 8] |= (unsigned char)(0x80 >> (x % 8));
            }
        }
        put32(f, 40); put32(f, s); put32(f, s * 2); put16(f, 1); put16(f, 32);
        put32(f, 0); put32(f, s * s * 4 + maskRow * s); put32(f, 0); put32(f, 0); put32(f, 0); put32(f, 0);
        fwrite(px, 4, (size_t)s * s, f);
        fwrite(mask, 1, (size_t)maskRow * s, f);
        free(px); free(mask);
    }
    fclose(f);
    return 0;
}
