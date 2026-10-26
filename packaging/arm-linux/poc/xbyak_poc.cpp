// PoC：xbyak_aarch64 生成 A64 代码 + memfd W^X 双映射执行
// 验证 v2 门槛②：GPU lib 侧 JIT 内存方案在 Android 沙箱内可用。
// 预期输出：sum=36 (1+2+...+8)
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <xbyak_aarch64/xbyak_aarch64.h>

using namespace Xbyak_aarch64;

// 生成 long sum8(const int* p)：累加 p[0..7]（模拟 SRT walker 的指针遍历+累加语义）
struct WalkerGen : Xbyak_aarch64::CodeGenerator {
    WalkerGen(uint8_t* buf, size_t size) : CodeGenerator(size, buf) {
        XReg src(0), acc(2), tmp(3);
        WReg wtmp(3);
        mov(acc, 0);
        for (int i = 0; i < 8; ++i) {
            ldr(wtmp, ptr(src, i * 4));
            sxtw(tmp, wtmp);
            add(acc, acc, tmp);
        }
        mov(x0, acc);
        ret();
    }
};

int main() {
    constexpr size_t kSize = 4096;

    // W^X：memfd 双映射——rw 别名生成（写），ex 别名执行
    int fd = memfd_create("xbyak_poc", 0);
    if (fd < 0) { perror("memfd_create"); return 1; }
    if (ftruncate(fd, kSize) != 0) { perror("ftruncate"); return 1; }
    auto* rw = (uint8_t*)mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    auto* ex = (uint8_t*)mmap(nullptr, kSize, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    if (rw == MAP_FAILED || ex == MAP_FAILED) { perror("mmap"); return 1; }

    // 生成（写 rw 别名）
    WalkerGen gen(rw, kSize);
    gen.setProtectModeRE();

    // 执行（读 ex 别名）
    int data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    using Fn = long (*)(const int*);
    auto fn = (Fn)ex;
    long sum = fn(data);

    printf("xbyak_aarch64 W^X PoC: sum=%ld (expect 36) %s\n",
           sum, sum == 36 ? "PASS" : "FAIL");
    return sum == 36 ? 0 : 1;
}
