/**
 * ============================================================================
 *  串口驱动 - USART1 初始化和中断服务
 * ============================================================================
 *  功能：
 *    1. 初始化USART1为921600bps, 8N1模式
 *    2. 提供Serial_SendByte()用于调试输出
 *    3. USART1_IRQHandler中断服务：接收PC端视频帧数据并写入OLED显存
 *
 *  通信参数：
 *    - 波特率：921600 bps（高速传输，保证视频帧率）
 *    - 数据位：8位
 *    - 停止位：1位
 *    - 校验位：无
 *    - 硬件流控：无
 *
 *  帧同步机制：
 *    PC端每帧发送前先发0xAA同步头。STM32收到0xAA时复位写入指针，
 *    确保后续1024字节数据从显存起始位置(0,0)开始写入。
 *    这种设计避免了帧错位导致的画面撕裂问题。
 *
 *  引脚映射：
 *    PA9  → USART1 TX（推挽复用输出）
 *    PA10 → USART1 RX（上拉输入）
 *
 *  作者：Abiubiu-z
 * ============================================================================
 */

#include "stm32f10x.h"      // STM32F10x标准外设库

/* 引用OLED模块的显存数组（定义在OLED.c中） */
extern uint8_t OLED_DisplayBuf[8][128];

/* 帧同步字节（必须与Python端FRAME_SYNC一致） */
#define FRAME_SYNC_BYTE     0xAA


/**
 * @brief  初始化USART1串口
 * @note   配置步骤：
 *         1. 使能USART1和GPIOA时钟
 *         2. 配置PA9为复用推挽输出（TX），PA10为上拉输入（RX）
 *         3. 配置USART1参数：921600bps, 8N1
 *         4. 使能接收中断（RXNE），配置NVIC
 *         5. 使能USART1外设
 */
void Serial_Init(void)
{
    /*---------- 使能外设时钟 ----------*/
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);  // USART1时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);   // GPIOA时钟

    /*---------- 配置GPIO ----------*/
    GPIO_InitTypeDef GPIO_InitStructure;

    // PA9: USART1 TX → 复用推挽输出（50MHz驱动能力）
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    // PA10: USART1 RX → 上拉输入（空闲时保持高电平，防止噪声误触发）
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /*---------- 配置USART1 ----------*/
    USART_InitTypeDef USART_InitStructure;
    USART_InitStructure.USART_BaudRate = 921600;                        // 波特率
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None; // 无流控
    USART_InitStructure.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;     // 收发双工
    USART_InitStructure.USART_Parity = USART_Parity_No;                 // 无校验
    USART_InitStructure.USART_StopBits = USART_StopBits_1;              // 1停止位
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;         // 8数据位
    USART_Init(USART1, &USART_InitStructure);

    /*---------- 使能接收中断 ----------*/
    // RXNE: Rx Not Empty，接收到数据时触发中断
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);

    /*---------- 配置中断优先级 ----------*/
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);     // 2位抢占优先级 + 2位子优先级

    NVIC_InitTypeDef NVIC_InitStructure;
    NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;                   // USART1全局中断
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;           // 抢占优先级1
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;                  // 子优先级1
    NVIC_Init(&NVIC_InitStructure);

    /*---------- 使能USART1 ----------*/
    USART_Cmd(USART1, ENABLE);
}


/**
 * @brief  通过USART1发送一个字节（调试用）
 * @param  Byte 要发送的字节数据
 * @note   阻塞式发送，等待发送完成寄存器(TXE)置位
 */
void Serial_SendByte(uint8_t Byte)
{
    USART_SendData(USART1, Byte);                                   // 写入发送数据寄存器
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);  // 等待发送完成
}


/**
 * @brief  USART1接收中断服务函数
 * @note   每收到1个字节触发一次中断
 *
 *  帧同步逻辑：
 *    1. 收到0xAA（帧同步字节）→ 复位写入指针(p0=0, p1=0)
 *       同步字节本身不存入显存，仅用于帧对齐
 *    2. 收到其他字节 → 存入OLED_DisplayBuf[p0][p1]，指针自增
 *    3. p1到达128（一页写满）→ p1归零，p0自增（进入下一页）
 *    4. p0到达8（全部8页写满）→ p0归零（循环覆盖）
 *
 *  显存布局（对应OLED_DisplayBuf[8][128]）：
 *    p0 (0~7)  = 页号（Page），每页8像素高
 *    p1 (0~127) = 列号（Column），每列1字节（8纵向像素）
 *
 *  数据流向：
 *    PC(COM5) → CH340 → USART1 → ISR → OLED_DisplayBuf → OLED_Update() → I2C → SSD1306
 */
void USART1_IRQHandler(void)
{
    /* 静态局部变量：保持跨中断调用的状态 */
    static uint8_t p0 = 0;  // 当前写入页号（0~7）
    static uint8_t p1 = 0;  // 当前写入列号（0~127）

    /* 检查是否为接收中断（RXNE: Rx Not Empty） */
    if (USART_GetITStatus(USART1, USART_IT_RXNE) == SET)
    {
        uint8_t RxData = USART_ReceiveData(USART1);     // 读取接收到的字节

        /*---------- 帧同步检测 ----------*/
        // 收到同步字节0xAA → 复位指针，开始新的一帧
        if (RxData == FRAME_SYNC_BYTE)
        {
            p0 = 0;     // 回到第0页
            p1 = 0;     // 回到第0列
        }
        /*---------- 正常数据存储 ----------*/
        else
        {
            // 将接收到的字节写入显存对应位置
            OLED_DisplayBuf[p0][p1] = RxData;

            // 列指针自增
            p1++;
            if (p1 >= 128)      // 当前页写满（128列）
            {
                p1 = 0;         // 列指针归零
                p0++;           // 进入下一页
                if (p0 >= 8)    // 全部8页写满
                {
                    p0 = 0;     // 页指针归零（循环覆盖，等待下一帧同步头）
                }
            }
        }

        /* 清除中断挂起位（必须操作，否则会反复触发中断） */
        USART_ClearITPendingBit(USART1, USART_IT_RXNE);
    }
}
