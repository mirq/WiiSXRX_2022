# AltiVec support for WiiStation (WarpOS/wos_av.c).
#
# long wos_av_call(void *fn, a1, a2, a3, a4, a5, a6)
# Calls fn(a1..a6) on a 16-byte aligned stack and returns its r3. WarpOS
# starts a task with a stack aligned to 4 bytes only (tools/avbench: mod 16
# = 12), and vector code built with -mabi=altivec keeps vector locals and
# its v20-v31 saves there with stvx/lvx, which drop the low 4 address bits.
        .text
        .balign 16
        .global wos_av_call
wos_av_call:
        mflr    0
        stw     0,4(1)
        mr      11,1
        clrrwi  12,1,4
        subi    12,12,48
        stw     11,0(12)
        mr      1,12
        mtctr   3
        mr      3,4
        mr      4,5
        mr      5,6
        mr      6,7
        mr      7,8
        mr      8,9
        bctrl
        lwz     1,0(1)
        lwz     0,4(1)
        mtlr    0
        blr
