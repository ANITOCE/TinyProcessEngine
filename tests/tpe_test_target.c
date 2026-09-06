/**
 * tpe_test_target.c — Linux 集成测试辅助进程
 *
 * 分配已知内存布局供测试验证:
 *   - heap int (0xDEADBEEF)
 *   - 匿名 mmap rw (PROT_READ|PROT_WRITE, 0xCAFEBABE)
 *   - 只读 mmap r  (PROT_READ, 不可写)
 *
 * 编译: gcc -o tpe_test_target tpe_test_target.c
 * 用法: ./tpe_test_target
 *   打印 PID 和各区域地址，然后 pause() 等待信号
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    // 1. Heap 分配
    int* heap_val = malloc(sizeof(int));
    *heap_val = 0xDEADBEEF;

    // 2. 匿名 rw mmap
    void* anon_rw = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (anon_rw != MAP_FAILED) {
        *(int*)anon_rw = 0xCAFEBABE;
    }

    // 3. 只读 mmap
    void* anon_ro = mmap(NULL, 4096, PROT_READ,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    printf("PID: %d\n", getpid());
    printf("heap:   %p = 0x%X\n", (void*)heap_val, *heap_val);
    printf("anon_rw: %p = 0x%X\n", anon_rw, (anon_rw != MAP_FAILED) ? *(int*)anon_rw : 0);
    printf("anon_ro: %p\n", anon_ro);
    fflush(stdout);

    // 保持运行直到被 SIGTERM 终止
    pause();
    return 0;
}
