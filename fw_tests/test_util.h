/**
  ******************************************************************************
  * @file    test_util.h
  * @brief   极简断言框架（不依赖任何第三方库，GCC / MSVC 都能编）
  *
  * 设计原则：
  *   - 一个断言只做一件事，失败时打印文件、行号、表达式和实际值；
  *   - 不捕获异常、不自动跳过：任何一个 CHECK 失败，进程退出码就是 1；
  *   - 全局计数放在 main.c 里定义，header 只做声明，避免多 TU 各有一份。
  ******************************************************************************
  */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>

extern int g_checks;
extern int g_fail;

#define SECTION(name) printf("\n[%s]\n", (name))

#define CHECK(cond)                                                      \
  do {                                                                   \
    g_checks++;                                                          \
    if (!(cond)) {                                                       \
      g_fail++;                                                          \
      printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                    \
  } while (0)

#define CHECK_EQ(actual, expect)                                         \
  do {                                                                   \
    long _a = (long)(actual);                                            \
    long _e = (long)(expect);                                            \
    g_checks++;                                                          \
    if (_a != _e) {                                                      \
      g_fail++;                                                          \
      printf("  FAIL  %s:%d  %s = %ld, 期望 %ld\n",                      \
             __FILE__, __LINE__, #actual, _a, _e);                       \
    }                                                                    \
  } while (0)

#endif /* TEST_UTIL_H */
