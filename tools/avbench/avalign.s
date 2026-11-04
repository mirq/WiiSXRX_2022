# void av_aligned(void (*fn)(), a1, a2, a3, a4, a5)
# Calls fn(a1, a2, a3, a4, a5) on a 16-byte aligned stack: WarpOS starts the
# task with a stack only 4-byte aligned, and vector code built with
# -mabi=altivec saves v20-v31 and keeps vector locals there with stvx,
# which drops the low 4 address bits.
        .text
        .balign 16
        .global av_aligned
av_aligned:
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
        bctrl
        lwz     1,0(1)
        lwz     0,4(1)
        mtlr    0
        blr
