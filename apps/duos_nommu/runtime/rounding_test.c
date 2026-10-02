/* Regression for the precise board failure; compile with SHIFT=16 and 23. */
#include "../../common/roi_threshold.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    const uint8_t in[3][5]={{125,125,173,173,173},{125,131,173,173,173},{131,126,173,173,173}};
    uint8_t out[3][4]; uint32_t columns[2];
    memset(out,0xa5,sizeof(out));
    assert(tt_roi_threshold(&in[0][0],5,&out[0][0],4,2,3,15,1,columns)==0);
    for (int y=0;y<3;++y) for (int x=0;x<2;++x) {
        unsigned sum=0;
        for (int dy=-7;dy<=7;++dy) for (int dx=-7;dx<=7;++dx) {
            int yy=y+dy,xx=x+dx;
            if (yy<0) yy=0;
            if (yy>2) yy=2;
            if (xx<0) xx=0;
            if (xx>1) xx=1;
            sum+=in[yy][xx];
        }
#if TT_THRESHOLD_FIXED_SHIFT == 16
        unsigned mean=((sum+113)*291)>>16;
#else
        unsigned mean=((sum+112)*37283)>>23;
#endif
        unsigned expected=mean>in[y][x]+1u ? 255 : 0;
        assert(out[y][x]==expected);
    }
    for (int y=0;y<3;++y) assert(out[y][2]==0xa5 && out[y][3]==0xa5);
    assert(out[0][1]==(TT_THRESHOLD_FIXED_SHIFT==23 ? 255 : 0));
    printf("rounding regression PASS shift=%u\n",TT_THRESHOLD_FIXED_SHIFT);
}
