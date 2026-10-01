#include "../../common/roi_threshold_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Independent direct window sum: validates the rolling-sum implementation,
 * output strides/guards, replicated borders, and threshold comparison. */
static uint8_t reference(const uint8_t *in, int stride, int w, int h,
                         int x, int y, int k, int threshold)
{
    int dx, dy, sum = 0, rx = w == 1 ? 0 : k / 2, ry = h == 1 ? 0 : k / 2;
    int area=(2*rx+1)*(2*ry+1), scale=0, delta=0;
    for (dy = -ry; dy <= ry; ++dy) for (dx = -rx; dx <= rx; ++dx) {
        int xx = x + dx, yy = y + dy;
        if (xx < 0) xx = 0;
        if (xx >= w) xx = w - 1;
        if (yy < 0) yy = 0;
        if (yy >= h) yy = h - 1;
        sum += in[yy * stride + xx];
    }
    /* Independent constants for the tested OpenCV small-area normalizers. */
    switch (area) {
        case 3: scale=21845; delta=2; break;
        case 5: scale=13107; delta=3; break;
        case 9: scale=7282; delta=4; break;
        case 15: scale=4369; delta=8; break;
        case 25: scale=2621; delta=13; break;
        case 31: scale=2114; delta=16; break;
        case 63: scale=1040; delta=32; break;
        case 225: scale=291; delta=113; break;
    }
    sum = (scale ? ((sum+delta)*scale)>>16 : (sum+area/2)/area) - in[y * stride + x];
    if (sum < 0) sum = 0;
    return sum > threshold ? 255 : 0;
}
int main(void)
{
    const int sizes[][2] = {{1,1},{1,19},{19,1},{2,3},{7,9},{31,17},{65,49}};
    const int kernels[] = {1,3,5,15,31,63};
    const int thresholds[] = {-1,0,1,3,127,254,255};
    uint32_t columns[TT_THRESHOLD_MAX_WIDTH], rng = 42;
    unsigned cases = 0;
    size_t si, ki, ti;
    int pattern, x, y;
    for (si=0; si<sizeof(sizes)/sizeof(sizes[0]); ++si)
        for (ki=0; ki<sizeof(kernels)/sizeof(kernels[0]); ++ki)
            for (ti=0; ti<sizeof(thresholds)/sizeof(thresholds[0]); ++ti)
                for (pattern=0; pattern<4; ++pattern) {
                    int w=sizes[si][0], h=sizes[si][1], stride=w+13, k=kernels[ki], t=thresholds[ti];
                    uint8_t *in=malloc(stride*h), *out=malloc(stride*h);
                    if (!in || !out) return 2;
                    memset(out, 0xa5, stride*h);
                    for (y=0; y<h; ++y) for (x=0; x<stride; ++x) {
                        rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5;
                        in[y*stride+x] = pattern==0 ? 0 : pattern==1 ? 255 :
                            pattern==2 ? ((x+y)&1)*255 : rng&255;
                    }
                    if (tt_roi_threshold(in,stride,out,stride,w,h,k,t,columns)) return 3;
                    for (y=0; y<h; ++y) for (x=0; x<stride; ++x) {
                        uint8_t wanted=x<w ? reference(in,stride,w,h,x,y,k,t) : 0xa5;
                        if (out[y*stride+x]!=wanted) {
                            fprintf(stderr,"mismatch: %dx%d k=%d t=%d (%d,%d)\n",w,h,k,t,x,y);
                            return 1;
                        }
                    }
                    free(in); free(out); ++cases;
                }
    if (!tt_roi_threshold(NULL,1,NULL,1,1,1,1,3,columns)) return 4;
    {
        const unsigned w=TT_THRESHOLD_MAX_WIDTH, h=TT_THRESHOLD_MAX_HEIGHT, stride=w+7;
        uint8_t *in=malloc(stride*h), *out=malloc(stride*h);
        if (!in || !out) return 5;
        memset(in,255,stride*h); memset(out,0xa5,stride*h);
        if (tt_roi_threshold(in,stride,out,stride,w,h,63,3,columns)) return 6;
        for (y=0; y<(int)h; ++y) for (x=0; x<(int)stride; ++x)
            if (out[y*stride+x] != (x<(int)w ? 0 : 0xa5)) return 7;
        if (!tt_roi_threshold(in,stride,out,stride,w,h,2,3,columns) ||
            !tt_roi_threshold(in,stride,out,stride,w+1,h,15,3,columns) ||
            !tt_roi_threshold(in,stride,out,stride,w,h+1,15,3,columns)) return 8;
        free(in); free(out);
    }
    printf("PASS: %u portable threshold cases; ABI sizes 64/64/128\n", cases);
    return 0;
}
