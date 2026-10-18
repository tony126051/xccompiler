#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "test_helper.h"
#include "test_helper.h" /* Test #pragma once duplicate inclusion */

/* Global variables & initializers */
int g_counter = 10;
int g_arr[4] = {10, 20, 30, 40};
const char *g_msg = "Hello from XCC!";

enum Color {
    COLOR_RED = 1,
    COLOR_GREEN,
    COLOR_BLUE = 10,
    COLOR_YELLOW
};

typedef struct Node {
    int val;
    struct Node *next;
} Node;

union WordBytes {
    int word;
    unsigned char bytes[4];
};

static int g_pass = 0;
static int g_fail = 0;

void check_int(const char *name, long actual, long expected) {
    if (actual == expected) {
        g_pass++;
    } else {
        printf("[FAIL] %s: expected %ld, got %ld\n", name, expected, actual);
        g_fail++;
    }
}

int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

void quicksort(int *arr, int low, int high) {
    if (low >= high) return;
    int pivot = arr[high];
    int i = low - 1;
    for (int j = low; j < high; j++) {
        if (arr[j] <= pivot) {
            i++;
            int tmp = arr[i];
            arr[i] = arr[j];
            arr[j] = tmp;
        }
    }
    int tmp = arr[i + 1];
    arr[i + 1] = arr[high];
    arr[high] = tmp;
    int pi = i + 1;
    quicksort(arr, low, pi - 1);
    quicksort(arr, pi + 1, high);
}

int sum_8_args(int a, int b, int c, int d, int e, int f, int g, int h) {
    return a + b + c + d + e + f + g + h;
}

void test_arithmetic_and_bitwise(void) {
    check_int("add/mul precedence", 2 + 3 * 4, 14);
    check_int("parentheses", (2 + 3) * 4, 20);
    check_int("sub/div/mod", 100 - 47 / 5 + 17 % 5, 93);
    check_int("unary neg", -(-42 + 10), 32);
    check_int("bitwise and/or/xor", (0xF0 & 0x3C) | (0x0F ^ 0x05), 0x3A);
    check_int("bit shift", (3 << 4) + (64 >> 2), 64);
    check_int("ternary true", (5 > 3) ? 111 : 222, 111);
    check_int("ternary false", (5 < 3) ? 111 : 222, 222);

    /* Short-circuit evaluation */
    int side = 0;
    int a = (0 && (++side));
    check_int("short-circuit && val", a, 0);
    check_int("short-circuit && side-effect", side, 0);
    int b = (1 || (++side));
    check_int("short-circuit || val", b, 1);
    check_int("short-circuit || side-effect", side, 0);

    /* Compound assignment and inc/dec */
    int x = 10;
    x += 5;
    x *= 2;
    x -= 6;
    x /= 3;
    check_int("compound assign", x, 8);
    check_int("post-inc", x++, 8);
    check_int("after post-inc", x, 9);
    check_int("pre-inc", ++x, 10);
    check_int("post-dec", x--, 10);
    check_int("pre-dec", --x, 8);
}

void test_control_flow(void) {
    /* while & break & continue */
    int sum = 0;
    int i = 0;
    while (i < 20) {
        i++;
        if (i % 2 == 0) continue;
        if (i > 11) break;
        sum += i; /* 1 + 3 + 5 + 7 + 9 + 11 = 36 */
    }
    check_int("while/break/continue", sum, 36);

    /* do-while */
    int dw = 0;
    do {
        dw += 7;
    } while (dw < 20);
    check_int("do-while", dw, 21);

    /* switch / case / default */
    int out = 0;
    for (int k = 0; k < 4; k++) {
        switch (k) {
            case 0: out += 1; break;
            case 1: out += 10; break;
            case 2: out += 100; break;
            default: out += 1000; break;
        }
    }
    check_int("switch-case", out, 1111);

    /* goto and label */
    int g = 1;
    goto skip_add;
    g += 999;
skip_add:
    g += 41;
    check_int("goto/label", g, 42);
}

void test_pointers_and_arrays(void) {
    int a = 100;
    int *p = &a;
    *p = 250;
    check_int("pointer deref write", a, 250);

    int **pp = &p;
    **pp = 500;
    check_int("double pointer", a, 500);

    int arr[5] = {11, 22, 33, 44, 55};
    int *ap = arr;
    check_int("ptr + 0", *ap, 11);
    check_int("ptr + 2", *(ap + 2), 33);
    ap += 3;
    check_int("ptr += 3", *ap, 44);
    check_int("ptr diff", (long)(ap - arr), 3);

    /* 2D array */
    int mat[2][3];
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 3; c++) {
            mat[r][c] = (r + 1) * 10 + c;
        }
    }
    check_int("2D array [0][2]", mat[0][2], 12);
    check_int("2D array [1][1]", mat[1][1], 21);

    /* Quicksort test */
    int data[8] = {64, 25, 12, 22, 11, 90, 3, 45};
    quicksort(data, 0, 7);
    check_int("quicksort[0]", data[0], 3);
    check_int("quicksort[3]", data[3], 22);
    check_int("quicksort[7]", data[7], 90);
}

void test_structs_unions_enums(void) {
    Vec2 v1 = {3, 4};
    Vec2 v2;
    v2 = v1; /* Struct copy assignment */
    v2.x += 7;
    check_int("struct init x", v1.x, 3);
    check_int("struct copy & modify", v2.x + v2.y, 14);

    /* Linked list with malloc/free and -> operator */
    Node *n1 = (Node *)malloc(sizeof(Node));
    Node *n2 = (Node *)malloc(sizeof(Node));
    Node *n3 = (Node *)malloc(sizeof(Node));
    n1->val = 10; n1->next = n2;
    n2->val = 20; n2->next = n3;
    n3->val = 30; n3->next = NULL;

    int list_sum = 0;
    for (Node *cur = n1; cur != NULL; cur = cur->next) {
        list_sum += cur->val;
    }
    check_int("linked list sum", list_sum, 60);
    free(n1);
    free(n2);
    free(n3);

    /* Union */
    union WordBytes wb;
    wb.word = 0;
    wb.bytes[0] = 0x78;
    wb.bytes[1] = 0x56;
    check_int("union shared storage", wb.word & 0xFFFF, 0x5678);

    /* Enum */
    check_int("enum RED", COLOR_RED, 1);
    check_int("enum GREEN", COLOR_GREEN, 2);
    check_int("enum BLUE", COLOR_BLUE, 10);
    check_int("enum YELLOW", COLOR_YELLOW, 11);
}

void test_preprocessor_and_globals(void) {
    check_int("macro constant", MAGIC_CONST, 1337);
    check_int("function-like macro ADD", ADD_MACRO(15, 27), 42);
    check_int("function-like macro SQR", SQR_MACRO(3 + 4), 49);

    check_int("global scalar", g_counter, 10);
    g_counter += 5;
    check_int("global scalar modified", g_counter, 15);
    check_int("global array[2]", g_arr[2], 30);
    check_int("global string strcmp", strcmp(g_msg, "Hello from XCC!"), 0);

    char buf[64];
    snprintf(buf, sizeof(buf), "%s %d", "XCC", 2026);
    check_int("variadic snprintf", strcmp(buf, "XCC 2026"), 0);
}

int main(void) {
    test_arithmetic_and_bitwise();
    test_control_flow();
    test_pointers_and_arrays();
    test_structs_unions_enums();
    test_preprocessor_and_globals();

    check_int("recursion fib(10)", fib(10), 55);
    check_int("8-arg call (register + stack)", sum_8_args(1, 2, 3, 4, 5, 6, 7, 8), 36);

    printf("Test Summary: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
