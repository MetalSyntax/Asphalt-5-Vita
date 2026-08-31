#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
int main() {
    printf("%08x\n", SCE_KERNEL_CPU_MASK_USER_ALL);
    printf("%08x\n", SCE_KERNEL_CPU_MASK_USER_2);
}
