/**
  ******************************************************************************
  * @file    stub_board.h
  * @brief   test_cmd.c 用的板级桩接口：捕获 app_cmd 发出的字节。
  ******************************************************************************
  */
#ifndef STUB_BOARD_H
#define STUB_BOARD_H

#include <stdint.h>

#define TX_CAP_MAX 512

extern uint8_t  tx_cap[TX_CAP_MAX];
extern uint16_t tx_cap_len;

void     tx_cap_reset(void);
uint8_t  tx_cap_at(uint16_t i);

/* 让测试可设定全局帧序号的起点，便于断言 ACK 帧头 SEQ */
void     frame_seq_reset(uint8_t start);

#endif /* STUB_BOARD_H */
