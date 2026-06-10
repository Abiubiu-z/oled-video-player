"""
================================================================================
 OLED视频播放器 - PC端（Python）
================================================================================
 功能：将MP4视频实时转换为128×64二值图像，通过串口发送到STM32驱动的OLED显示屏
 硬件：PC → CH340(USB转串口) → STM32F103C8 → 0.96寸OLED(SSD1306/I2C)
 通信：UART 921600bps，每帧1字节同步头(0xAA) + 1024字节图像数据
 作者：Abiubiu-z
================================================================================
"""

import numpy as np
import cv2
import serial
import time
import os

# ============================================================================
# 全局配置
# ============================================================================

# 串口配置
SERIAL_PORT = 'COM5'        # 串口号（CH340 USB转串口）
SERIAL_BAUD = 921600        # 波特率（必须与STM32端一致）

# OLED屏幕分辨率
OLED_WIDTH = 128            # 屏幕宽度（像素）
OLED_HEIGHT = 64            # 屏幕高度（像素）
OLED_PAGES = 8              # 页数（每页8像素高，64/8=8页）

# 图像处理参数
BINARY_THRESHOLD = 170      # 二值化阈值（0~255，越高画面越暗）

# 帧同步字节（必须与STM32端Serial.c中定义一致）
FRAME_SYNC = 0xAA


# ============================================================================
# 图像转换函数
# ============================================================================

def img2array(frame):
    """
    将128×64的二值图像转换为OLED显存数组格式

    OLED显存布局（SSD1306）：
      - 屏幕被分为8个"页"（Page），每页8像素高
      - 每页有128列（Column），每列用1个字节表示8个纵向像素
      - 字节的Bit0对应页内最上方像素，Bit7对应最下方像素
      - 数据按页优先存储：先存Page0的128字节，再Page1的128字节...

    参数:
        frame: 64×128的二值图像（numpy数组，值0或255）
    返回:
        array: 8×128的uint8数组，可直接发送到OLED显存
    """
    array = np.zeros((OLED_PAGES, OLED_WIDTH), dtype='uint8')

    for j in range(OLED_HEIGHT):        # 遍历每一行（0~63）
        for i in range(OLED_WIDTH):     # 遍历每一列（0~127）
            if frame[j][i] > 0:         # 该像素为白色（点亮）
                # 设置对应页对应列的指定位
                # j // 8：该行属于第几页（0~7）
                # j % 8：  该行在页内的第几位（0~7）
                # 0x01 << (j % 8)：将Bit0移到目标位位置
                array[j // 8][i] = array[j // 8][i] | (0x01 << (j % 8))

    return array


# ============================================================================
# 主程序
# ============================================================================

def main():
    """主函数：打开视频，逐帧处理并通过串口发送到OLED"""

    # ---- 切换到脚本所在目录（确保相对路径引用视频文件正常）----
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    print(f"[信息] 工作目录: {script_dir}")

    # ---- 打开串口 ----
    print(f"[信息] 正在打开串口 {SERIAL_PORT} @ {SERIAL_BAUD} bps...")
    serial_port = serial.Serial(SERIAL_PORT, SERIAL_BAUD, write_timeout=1)
    print(f"[信息] 串口已打开: {serial_port.is_open}")

    # ---- 打开视频文件 ----
    video_file = '小猫.mp4'
    print(f"[信息] 正在打开视频: {video_file}")
    cap = cv2.VideoCapture(video_file)

    if not cap.isOpened():
        print(f"[错误] 无法打开视频文件 '{video_file}'！请确认文件存在且为有效视频格式。")
        serial_port.close()
        return

    fps = cap.get(cv2.CAP_PROP_FPS)
    total_frames = cap.get(cv2.CAP_PROP_FRAME_COUNT)
    print(f"[信息] 视频就绪: {fps:.0f} FPS, 共 {total_frames:.0f} 帧, "
          f"时长约 {total_frames/fps:.1f} 秒")

    # ---- 创建预览窗口 ----
    cv2.namedWindow('OLED 预览 (按Q退出)', cv2.WINDOW_NORMAL)
    cv2.resizeWindow('OLED 预览 (按Q退出)', OLED_WIDTH * 4, OLED_HEIGHT * 4)

    # ---- 逐帧播放循环 ----
    start_time = time.time()
    frame_count = 0

    print("[信息] 开始播放，按 Q 键退出...")
    print("-" * 50)

    while cap.isOpened():
        # 根据实际流逝时间计算当前应播放到哪一帧
        # 这样即使帧处理有延迟，也能保持音画同步
        elapsed_time = time.time() - start_time
        target_frame = int(elapsed_time * fps)

        # 定位到目标帧
        cap.set(cv2.CAP_PROP_POS_FRAMES, target_frame)
        success, frame = cap.read()

        if not success:
            print(f"[信息] 视频播放结束（{elapsed_time:.1f}秒，{frame_count}帧）")
            break

        # ---- 图像处理流水线 ----
        # ① 缩放到OLED分辨率 128×64
        frame = cv2.resize(frame, (OLED_WIDTH, OLED_HEIGHT))

        # ② 转为灰度图（RGB → 单通道亮度）
        frame = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

        # ③ 二值化（灰度 → 纯黑白）
        #   THRESH_BINARY: 像素值>阈值→255(白), 否则→0(黑)
        frame = cv2.threshold(frame, BINARY_THRESHOLD, 255, cv2.THRESH_BINARY)[1]

        # ---- 显示预览 ----
        cv2.imshow('OLED 预览 (按Q退出)', frame)

        # ---- 转换为OLED格式并通过串口发送 ----
        oled_data = img2array(frame)

        # 先发送帧同步字节，告诉STM32新的一帧开始了
        serial_port.write(bytes([FRAME_SYNC]))

        # 再发送1024字节图像数据（8页 × 128列）
        serial_port.write(oled_data.tobytes())

        # ---- 进度输出 ----
        frame_count += 1
        if frame_count == 1:
            print(f"[信息] 首帧已发送（{1 + OLED_PAGES * OLED_WIDTH} 字节/帧）")
        if frame_count % 30 == 0:
            print(f"[进度] 已发送 {frame_count} 帧 "
                  f"({elapsed_time:.1f}s / {total_frames/fps:.1f}s)")

        # ---- 键盘控制 ----
        # waitKey(1): 等待1ms，同时用于cv2窗口刷新
        key = cv2.waitKey(1)
        if key & 0xFF == ord('q'):      # 按 Q 键退出
            print("[信息] 用户按下Q键，退出播放")
            break

    # ---- 清理资源 ----
    print("-" * 50)
    print(f"[信息] 播放完毕，共发送 {frame_count} 帧")
    cap.release()
    cv2.destroyAllWindows()
    serial_port.close()
    print("[信息] 资源已释放，程序退出")


# ============================================================================
# 程序入口
# ============================================================================

if __name__ == '__main__':
    main()
