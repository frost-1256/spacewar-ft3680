#pragma once
#define FT_PINS_UNKNOWN -1
#define FT_PINS_HEALTHY 0
#define FT_PINS_REPAIR 1
/* Windows ULONG is 32 bits on both host x64 and target ARM64. No MMIO here. */
static __inline int FtClassifyPins(const unsigned long Ctl[4])
{
    const unsigned long expected[4]={1,0x200,0x201,0x201};
    unsigned long i;
    int changed=0;
    for(i=0;i<4;++i) {
        unsigned long v=Ctl[i]&0x23f;
        if(v==expected[i]) continue;
        if(v==5 || (i>0 && v==0x205)) changed=1;
        else return FT_PINS_UNKNOWN;
    }
    return changed ? FT_PINS_REPAIR : FT_PINS_HEALTHY;
}
