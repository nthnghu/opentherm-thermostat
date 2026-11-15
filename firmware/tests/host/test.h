#pragma once
#include <stdio.h>
#include <math.h>

static int g_fail, g_pass;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (cond) {                                                        \
            g_pass++;                                                      \
        } else {                                                           \
            g_fail++;                                                      \
            printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);       \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)
#define NEAR(a, b, tol) (fabs((double)(a) - (double)(b)) <= (tol))
#define TEST_DONE()                                                        \
    do {                                                                   \
        printf("%d passed, %d failed\n", g_pass, g_fail);                  \
        return g_fail ? 1 : 0;                                             \
    } while (0)
