/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2026 Norinq <qq984487046@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#ifndef __DSLOGIC_U3PRO32_PROTOCOL_H_
#define __DSLOGIC_U3PRO32_PROTOCOL_H_

#include <stdint.h>
#include <glib.h>
#include <libusb.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"

#define LOG_PREFIX "dreamsourcelab-dslogic-u3pro32"
#define U3_CHANNELS 32
// 每通道以 64 个采样位为一组传输；最大组长为 32 * 8 字节。
#define U3_ATOMIC_SAMPLES 64
#define U3_GROUP_BYTES (U3_CHANNELS * 8)
#define U3_BUFFER_BYTES (256 * 1024)
#define U3_HEADER_BYTES 1024
#define U3_USB_TIMEOUT 3000
// reference 型号表以 bit 给出 2 Gibit 存储容量；RLE 最多扩展 1024 倍。
#define U3_MEMORY_BITS (UINT64_C(2) * 1024 * 1024 * 1024)
#define U3_RLE_FACTOR 1024
// Buffer 硬件计数按 64 样本对齐，线上计数字段宽 32 bit（单位 16 样本）。
#define U3_MAX_CAPTURE_SAMPLES (UINT64_C(0xffffffff) * 16)
// DSL 厂商命令目标号，沿用参考驱动 command.h 的协议定义。
enum {
	U3_CTL_STATUS = 2, U3_CTL_PROG = 3, U3_CTL_LED = 5,
	U3_CTL_INTRDY = 6, U3_CTL_WORDWIDE = 7, U3_CTL_START = 8,
	U3_CTL_STOP = 9, U3_CTL_BULK = 10, U3_CTL_REG = 14,
};

// 上下文由标准设备清理函数释放；transfer 在最终回调中释放。
struct dev_context {
	// 借用公共上下文；本驱动不得释放该指针。
	struct sr_context *ctx;
	uint64_t samplerate; // Hz，500 MHz 时钟按整数分频。
	uint64_t max_samplerate; // USB 链路允许的全通道流式上限，Hz。
	uint64_t limit_samples; // 每通道请求的样本数；Buffer 会向上对齐到 64 样本。
	// FALSE 为默认 Stream，TRUE 为先存储后上传的硬件 Buffer。
	gboolean buffer_mode;
	// 仅 Buffer 允许 RLE；FPGA 在上传时恢复普通交织位流。
	gboolean rle;
	uint64_t capture_samples; // Buffer 编程的对齐计数，单位为每通道样本。
	uint64_t received_bytes; // 本次收到的有效数据字节数，不含采集头。
	uint64_t expected_bytes; // Buffer 头部确认的上传数据字节数。
	// 独立连续开关；启用时保留但不应用 limit_samples。
	gboolean continuous;
	// 显式 continuous=true 优先于样本缓存；需显式 false 才撤销此意图。
	gboolean continuous_requested;
	// 已向会话成功提交的每通道采样数。
	uint64_t sent_samples;
	double threshold; // 逻辑电平判定阈值，单位 V。
	// 保存扫描时的链路速度，用于选择 GPIF 字宽握手。
	int usb_speed;
	// 从提交传输成功到完成回调清理结束期间为 TRUE。
	gboolean running;
	// 停止请求的幂等标记；阻止接收回调重提交传输。
	gboolean stopping;
	// 第一笔读取必须校验设备头部，之后才解析通道位流。
	gboolean header_pending;
	// 仅移除本次成功添加的 USB 事件源。
	gboolean source_added;
	// 唯一在途传输；释放前必须收到完成或取消回调。
	struct libusb_transfer *transfer;
	uint32_t channel_mask; // 本次采集启用的物理通道快照。
	// 快照内置位数，决定一个紧凑原子组的字节数。
	unsigned int channel_count;
	// 数据阶段实际请求长度，单位字节；按 USB 包对齐，不超过分配上限。
	unsigned int transfer_size;
	// 未满一个原子组的残片字节数，启动时清零。
	unsigned int partial_length;
	uint8_t partial[U3_GROUP_BYTES]; // 保存跨 USB 传输的未完整采样组。
	uint8_t output[U3_ATOMIC_SAMPLES * 4]; // 小端、每采样四字节。
};

SR_PRIV int U3Write(const struct sr_dev_inst *sdi, uint8_t dest,
	uint16_t offset, const uint8_t *data, uint8_t size);
SR_PRIV int U3Read(const struct sr_dev_inst *sdi, uint8_t dest,
	uint8_t *data, uint8_t size);
SR_PRIV int U3Open(struct sr_dev_inst *sdi);
SR_PRIV int U3Threshold(const struct sr_dev_inst *sdi, double threshold);
SR_PRIV int U3Start(const struct sr_dev_inst *sdi);
SR_PRIV int U3Stop(struct sr_dev_inst *sdi);

#endif
