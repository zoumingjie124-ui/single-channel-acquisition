/**
  ******************************************************************************
  * @file    app_types.h
  * @brief   跨模块共享的类型定义：告警级别。
  *
  * 为什么单独一个文件：
  *   app_alarm / app_task / app_cmd / app_proto 都要用到 app_level_t，
  *   放在任意一个模块的头文件里都会造成互相包含，单独抽出来最干净。
  ******************************************************************************
  */
#ifndef __APP_TYPES_H
#define __APP_TYPES_H

#include <stdint.h>

/* 告警级别。数值大小即严重程度，比较时可直接用 */
typedef enum
{
  APP_LEVEL_NORMAL = 0,
  APP_LEVEL_WARN   = 1,
  APP_LEVEL_CRIT   = 2
} app_level_t;

#endif /* __APP_TYPES_H */
