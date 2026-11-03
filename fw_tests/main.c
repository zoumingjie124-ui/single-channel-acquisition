/**
  ******************************************************************************
  * @file    main.c
  * @brief   固件纯逻辑模块的 PC 端单元测试入口
  *
  * 被测源码（就是仓库里的真实文件，不复制、不重写）：
  *     ../App/Src/app_proto.c
  *     ../App/Src/app_alarm.c
  *     ../App/Src/app_filter.c
  *
  * 这三个模块不依赖 HAL，可以原样在 PC 上编译运行。
  * 与上位机的 Python 测试（host/tests）分开：那是测协议解析/流水线，
  * 这里是测**设备侧真正跑的那份 C 代码**。
  ******************************************************************************
  */
#include "test_util.h"

int g_checks = 0;
int g_fail   = 0;

void test_proto(void);
void test_alarm(void);
void test_filter(void);

int main(void)
{
  printf("============================================================\n");
  printf("固件纯逻辑单元测试（GCC / PC）\n");
  printf("被测：app_proto.c  app_alarm.c  app_filter.c\n");
  printf("============================================================\n");

  test_proto();
  test_alarm();
  test_filter();

  printf("\n============================================================\n");
  printf("断言 %d 项，失败 %d 项\n", g_checks, g_fail);
  printf("RESULT: %s\n", (g_fail == 0) ? "PASS" : "FAIL");
  printf("============================================================\n");

  return (g_fail == 0) ? 0 : 1;
}
