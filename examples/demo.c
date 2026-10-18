#include <stdio.h>
#include <stdlib.h>

#define MAX_ITEMS 6

typedef struct Point {
    int x;
    int y;
} Point;

int manhattan_dist(Point *a, Point *b) {
    int dx = a->x - b->x;
    int dy = a->y - b->y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx + dy;
}

int factorial(int n) {
    return (n <= 1) ? 1 : n * factorial(n - 1);
}

int main(void) {
    printf("=== XCC Cross-Platform C Compiler Demo ===\n");

#if defined(__aarch64__)
    const char *arch = "ARM64 (AArch64)";
#elif defined(__x86_64__)
    const char *arch = "x86_64 (AMD64)";
#else
    const char *arch = "Unknown";
#endif

#if defined(__APPLE__)
    const char *os = "macOS (Mach-O)";
#elif defined(__linux__)
    const char *os = "Linux (ELF)";
#elif defined(_WIN32)
    const char *os = "Windows (PE/COFF)";
#else
    const char *os = "Unknown OS";
#endif

    printf("Target Architecture : %s\n", arch);
    printf("Target OS           : %s\n", os);

    Point origin = {0, 0};
    Point target = {12, -5};
    printf("Manhattan distance  : %d\n", manhattan_dist(&origin, &target));

    printf("Factorials (1..%d)   : ", MAX_ITEMS);
    for (int i = 1; i <= MAX_ITEMS; i++) {
        printf("%d! = %d%s", i, factorial(i), (i == MAX_ITEMS) ? "\n" : ", ");
    }

    return 0;
}
