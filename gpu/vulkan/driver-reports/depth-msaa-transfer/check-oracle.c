#include "oracle.h"
#include <stdio.h>
int main(void) {
    uint64_t mappings=0; unsigned covered=0;
    for(uint32_t y=0;y<DP_H;++y) for(uint32_t x=0;x<DP_W;++x) for(uint32_t s=0;s<2;++s) {
        uint32_t ss=dp_source_sample(x,s); if(ss>3 || (x>>1)>=DP_SW) return 1;
        uint32_t d=dp_float_bits(dp_pattern_depth(x>>1,y,ss));
        if(d<0x3E000000u || d>0x3F100000u || dp_pattern_stencil(x>>1,y,ss)>255u) return 1;
        covered |= 1u<<ss; ++mappings;
    }
    if(dp_source_sample(0,0)!=2 || dp_source_sample(1,0)!=3 || dp_source_sample(0,1)!=0 || dp_source_sample(1,1)!=1) return 1;
    if(dp_float_bits(dp_pattern_depth(0,0,2))!=0x3F000000u || dp_pattern_stencil(0,0,2)!=58) return 1;
    if(covered!=15 || dp_float_bits(0.25f)!=0x3E800000u || dp_color(0)!=0xFF0000FFu || dp_color(1)!=0xFF00FF00u || dp_color(2)!=0xFFFF0000u) return 1;
    printf("PASS host oracle mappings=%llu source_samples=0,1,2,3; PS5 NOT TESTED\n",(unsigned long long)mappings);
    return 0;
}
