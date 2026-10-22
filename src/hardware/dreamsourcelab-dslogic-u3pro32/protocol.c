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


#include <config.h>
#include "protocol.h"
#include <string.h>

// 来源：reference/hardware/DSL/{command.h,dsl.c,dsl.h}。
// Copyright (C) 2013 Bert Vermeulen <bert@biot.com>
// 同时保留本文件上方 GPL-3.0-or-later 许可声明。

/**
 * @authors Codex
 * @brief 构造显式小端命令头，避免结构体填充及主机字节序影响。
 * @details 命令头依次为目标、偏移低字节、偏移高字节和负载长度。
 * 发送长度必须包含四字节命令头；USB 非负返回值不保证完整发送。
 * 所有同步控制访问由会话线程串行执行，不提供跨线程锁。
 * @param sdi 已打开的设备，不得为 NULL。
 * @param dest DSL 目标编号。
 * @param offset 目标内的字节偏移。
 * @param data 借用的负载；size 为零时允许 NULL。
 * @param size 负载字节数，不超过 60。
 * @return SR_OK 或完整长度检查、USB 错误。
 */
SR_PRIV int U3Write(const struct sr_dev_inst *sdi, uint8_t dest,
	uint16_t offset, const uint8_t *data, uint8_t size)
{
	struct sr_usb_dev_inst *usb = sdi->conn;
	// USB 控制端点最多携带四字节头和六十字节负载。
	uint8_t cmd[64] = { dest, offset & 0xff, offset >> 8, size };
	int ret = 0;

	if (!usb->devhdl || (size > 60) || (size && !data))
		return SR_ERR_ARG;

	if (size)
		memcpy(cmd + 4, data, size);

	ret = libusb_control_transfer(usb->devhdl,
		LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
		0xb0, 0, 0, cmd, size + 4, U3_USB_TIMEOUT);
	if (ret != size + 4) {
		sr_err("DSL control write failed: target %u, result %d.", dest, ret);
		return SR_ERR;
	}

	return SR_OK;
}

/**
 * @authors Codex
 * @brief 先选定目标，再执行厂商控制读取；拒绝短读。
 * @details 准备请求和读取请求必须连续执行，期间不能插入其他目标读取。
 * 该实现仅供从偏移零开始的状态读取，不能用于任意寄存器地址。
 * 输出缓冲区由调用方保有；读取失败时内容不可作为有效状态使用。
 * @param sdi 已打开的设备，不得为 NULL。
 * @param dest DSL 目标编号。
 * @param data 调用方提供的输出缓冲区，不得为 NULL。
 * @param size 输出缓冲区大小，单位字节。
 * @return SR_OK 或 USB 错误；同步调用不适用于中断上下文。
 */
SR_PRIV int U3Read(const struct sr_dev_inst *sdi, uint8_t dest,
	uint8_t *data, uint8_t size)
{
	struct sr_usb_dev_inst *usb = sdi->conn;
	// 读取准备头不含数据；随后的 IN 请求才写入调用方缓冲区。
	uint8_t header[4] = { dest, 0, 0, size };
	int ret = 0;

	// 状态读取也检查调用约束，防止错误路径使用已关闭的句柄。
	if (!usb->devhdl || !data || !size)
		return SR_ERR_ARG;

	ret = libusb_control_transfer(usb->devhdl,
		LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
		0xb1, 0, 0, header, sizeof(header), U3_USB_TIMEOUT);
	if (ret != sizeof(header)) {
		sr_err("DSL read preparation failed: target %u, result %d.", dest, ret);
		return SR_ERR;
	}

	// 与 DSView 一致，给固件 10 ms 准备返回数据，避免紧接读取导致失败。
	g_usleep(10 * 1000);

	ret = libusb_control_transfer(usb->devhdl,
		LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN,
		0xb2, 0, 0, data, size, U3_USB_TIMEOUT);
	if (ret != size) {
		sr_err("DSL control read failed: target %u, result %d.", dest, ret);
		return SR_ERR;
	}

	return SR_OK;
}

/**
 * @authors Codex
 * @brief 等待状态位，给参考驱动的无限等待增加三秒截止时间。
 * @details 截止时间使用单调时钟，不受系统时间调整影响。
 * 控制传输自身另有超时，单次失败立即返回而不继续轮询。
 * 每次未就绪后让出一毫秒，避免忙等持续占用 CPU。
 * @param sdi 已打开的设备。
 * @param mask 必须置位的硬件状态掩码。
 * @return SR_OK、超时或控制读取错误。
 */
static int WaitStatus(const struct sr_dev_inst *sdi, uint8_t mask)
{
	int64_t deadline = g_get_monotonic_time() + (U3_USB_TIMEOUT * 1000);
	uint8_t status = 0;

	do {
		if (U3Read(sdi, U3_CTL_STATUS, &status, 1) != SR_OK)
			return SR_ERR;

		if ((status & mask) == mask)
			return SR_OK;

		g_usleep(1000);
	} while (g_get_monotonic_time() < deadline);

	sr_err("Timed out waiting for DSL status 0x%02x.", mask);
	return SR_ERR;
}

/**
 * @authors Codex
 * @brief 写一个控制字节，复用完整命令长度检查。
 * @details 适用于无地址偏移的控制目标，不能代替带偏移的寄存器写入。
 * 临时字节仅在本次同步调用期间借给 U3Write。
 * 位掩码的实际含义由上层配置或握手步骤决定。
 * @param sdi 已打开的设备。
 * @param dest DSL 目标。
 * @param value 字节值。
 * @return SR_OK 或 USB 错误。
 */
static int WriteByte(const struct sr_dev_inst *sdi, uint8_t dest, uint8_t value)
{
	return U3Write(sdi, dest, 0, &value, 1);
}

/**
 * @authors Codex
 * @brief 宣告批量写入长度，再通过 OUT 端点 2 发送。
 * @details FPGA 镜像按字节声明长度，采集基础设置按 16 位字声明长度。
 * 命令长度字段只有 24 位；镜像大小在资源加载时限制。
 * 传输不足声明长度时直接报错，避免继续错误的配置握手。
 * @param sdi 已打开的设备。
 * @param data 调用方持有的配置数据，调用期间保持有效。
 * @param size 批量负载字节数。
 * @param words TRUE 时命令中的长度以 16 位字为单位。
 * @return SR_OK 或 USB 错误。
 */
static int BulkWrite(const struct sr_dev_inst *sdi, uint8_t *data,
	unsigned int size, gboolean words)
{
	struct sr_usb_dev_inst *usb = sdi->conn;
	unsigned int count = words ? (size / 2) : size;
	// 24 位长度按低字节先行，与参考固件的控制命令布局一致。
	uint8_t length[3] = { count & 0xff, (count >> 8) & 0xff, count >> 16 };
	int actual = 0, ret = 0;

	if (U3Write(sdi, U3_CTL_BULK, 0, length, sizeof(length)) != SR_OK)
		return SR_ERR;

	// FPGA 配置与采集设置的握手不同；仅采集设置等待 SYS_CLR。
	if (words && (WaitStatus(sdi, 1 << 3) != SR_OK))
		return SR_ERR;

	ret = libusb_bulk_transfer(usb->devhdl, 2, data, size,
		&actual, U3_USB_TIMEOUT);
	return ((ret == 0) && (actual == (int)size)) ? SR_OK : SR_ERR;
}

/**
 * @authors Codex
 * @brief 从 libsigrok 固件资源加载本型号 FPGA 镜像并完成配置握手。
 * @details 镜像名称统一为 dreamsourcelab-dslogic-u3pro32-fpga.fw，避免与其他型号混用。
 * 资源系统决定搜索路径，因此不依赖开发机的绝对固件路径。
 * 仅在硬件 FPGA_DONE 未置位时加载，避免重复复位已配置 FPGA。
 * @param sdi 已打开且已声明接口的设备。
 * @return SR_OK 或固件缺失、USB、状态超时错误。
 * @note 不使用旧款 FX2 固件上传协议；镜像内存在所有路径释放。
 */
static int ConfigureFpga(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	size_t size = 0;
	uint8_t *image = NULL;
	int ret = SR_ERR;

	image = sr_resource_load(devc->ctx, SR_RESOURCE_FIRMWARE,
		"dreamsourcelab-dslogic-u3pro32-fpga.fw", &size, 0xffffff);
	if (!image)
		return SR_ERR;

	if (!size)
		goto done;

	if ((WriteByte(sdi, U3_CTL_PROG, (uint8_t)~(1 << 2)) != SR_OK) ||
		(WriteByte(sdi, U3_CTL_LED, 0xfc) != SR_OK) ||
		(WriteByte(sdi, U3_CTL_PROG, 1 << 2) != SR_OK) ||
		(WaitStatus(sdi, 1 << 5) != SR_OK) ||
		(WriteByte(sdi, U3_CTL_INTRDY, 0x7f) != SR_OK) ||
		(BulkWrite(sdi, image, size, FALSE) != SR_OK) ||
		(WriteByte(sdi, U3_CTL_INTRDY, 0x80) != SR_OK) ||
		(WaitStatus(sdi, 0x80) != SR_OK) ||
		(WriteByte(sdi, U3_CTL_INTRDY, 0x7f) != SR_OK) ||
		(WaitStatus(sdi, 0x40) != SR_OK) ||
		(WriteByte(sdi, U3_CTL_LED, 1) != SR_OK) ||
		(WriteByte(sdi, U3_CTL_WORDWIDE, 1) != SR_OK))
		goto done;

	ret = SR_OK;
done:
	g_free(image);
	image = NULL;
	return ret;
}

/**
 * @authors Codex
 * @brief 设置比较阈值；换算沿用 U3Pro32 的参考 DAC 比例。
 * @details DAC 寄存器位于 I2C_REG 目标的 0x78 偏移处。
 * 此寄存器面向整机比较器，因此全部逻辑通道共用一个阈值。
 * 配置缓存由调用方在写入成功后更新，避免缓存与硬件状态不一致。
 * @param sdi 已打开设备。
 * @param threshold 阈值，单位 V，调用方已校验为 0 至 5 V。
 * @return SR_OK 或寄存器写入错误。
 */
SR_PRIV int U3Threshold(const struct sr_dev_inst *sdi, double threshold)
{
	uint8_t value = (uint8_t)(threshold / 3.3 * (1.5 / 2.5) * 255.0);

	return U3Write(sdi, U3_CTL_REG, 0x78, &value, 1);
}

/**
 * @authors Codex
 * @brief 打开接口、检查 FPGA 状态并初始化 500 MHz ADF4360 时钟。
 * @details 先检查 USB 配置并读取固件版本，再按 DSView 顺序申请接口。
 * 配置零时报告主机枚举失败；USB/IP 的配置请求可能在内核中阻塞。
 * 设备必须已运行能响应 0xb0 至 0xb2 请求的 DSL 固件。
 * 不向本型号发送 FX2 的 CPU 复位或 RAM 下载请求。
 * ADF 串行配置按参考驱动逐字节写入，不能合并为连续地址写入。
 * @param sdi 扫描创建的设备，不得为 NULL。
 * @return SR_OK 或错误；失败时关闭句柄，不遗留已占用接口。
 */
SR_PRIV int U3Open(struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	struct sr_usb_dev_inst *usb = sdi->conn;
	uint8_t status = 0, value = 0;
	uint8_t version[2] = { 0, 0 };
	// ADF4360-7 的控制、R 计数器及 N 计数器串行字节序列。
	const uint8_t clock[][4] = {
		{ 0x01, 0x61, 0x00, 0x30 },
		{ 0x01, 0x40, 0xf1, 0x46 },
		{ 0x01, 0x62, 0x3d, 0x40 },
	};
	unsigned int i = 0, j = 0;
	int ret = LIBUSB_SUCCESS;
	int configuration = 0;

	if (sr_usb_open(devc->ctx->libusb_ctx, usb) != SR_OK)
		return SR_ERR;

	// 首次 USB/IP 挂接可能因 SET_CONFIGURATION 超时而留下配置零。
	// 已实测配置重试可阻塞在内核取消路径，因此仅查询，不自动重设配置。
	ret = libusb_get_configuration(usb->devhdl, &configuration);
	if (ret != LIBUSB_SUCCESS) {
		sr_err("Unable to query U3Pro32 USB configuration: %s (%d).",
			libusb_error_name(ret), ret);
		goto close;
	}

	sr_info("U3Pro32 USB %u.%u configuration %d.",
		usb->bus, usb->address, configuration);
	if (configuration == 0) {
		sr_err("U3Pro32 has no active USB configuration; interface 0 is unavailable.");
		// 区分设备打开成功与主机配置失败，提示检查首次连接的实际错误。
		sr_err("USB enumeration did not activate a configuration; check host USB logs for SET_CONFIGURATION failures.");
		sr_err("If using USB/IP, reconnect the device on the exporting host and reattach it before retrying.");
		goto close;
	}

	// DSL 目标 0 为控制器版本；与 DSView 一致，先验证通信再申请接口。
	if (U3Read(sdi, 0, version, sizeof(version)) != SR_OK) {
		sr_err("Unable to read U3Pro32 controller firmware version.");
		goto close;
	}

	sr_info("U3Pro32 controller firmware %u.%u.", version[0], version[1]);

	// 保留参考驱动对接口暂不可见的单次重试，不重设 USB 配置或复位设备。
	ret = libusb_claim_interface(usb->devhdl, 0);
	if (ret == LIBUSB_ERROR_NOT_FOUND)
		ret = libusb_claim_interface(usb->devhdl, 0);

	if (ret != LIBUSB_SUCCESS) {
		sr_err("Unable to claim U3Pro32 interface 0: %s (%d).",
			libusb_error_name(ret), ret);
		goto close;
	}

	if (U3Read(sdi, U3_CTL_STATUS, &status, 1) != SR_OK) {
		sr_err("DSL firmware is unavailable; initialize the device firmware first.");
		goto release;
	}

	sr_info("U3Pro32 hardware status 0x%02x, FPGA %s.", status,
		(status & 0x40) ? "configured" : "requires configuration");

	if (!(status & 0x40) && (ConfigureFpga(sdi) != SR_OK))
		goto release;

	if ((U3Write(sdi, U3_CTL_REG, 0x70, &value, 1) != SR_OK) ||
		(U3Threshold(sdi, devc->threshold) != SR_OK))
		goto release;

	value = 1;
	if (U3Write(sdi, U3_CTL_REG, 0x4a, &value, 1) != SR_OK)
		goto release;

	for (i = 0; i < ARRAY_SIZE(clock); i++) {
		// 参考硬件要求在最后一个锁相配置字之前等待 10 ms。
		if (i == 2)
			g_usleep(10000);

		for (j = 0; j < ARRAY_SIZE(clock[0]); j++) {
			if (U3Write(sdi, U3_CTL_REG, 0x48, &clock[i][j], 1) != SR_OK)
				goto release;
		}
	}

	return SR_OK;
release:
	libusb_release_interface(usb->devhdl, 0);
close:
	libusb_close(usb->devhdl);
	usb->devhdl = NULL;
	return SR_ERR;
}

/**
 * @authors Codex
 * @brief 向字节数组写入一个小端 16 位配置字段。
 * @details 该函数不要求输出地址按 16 位边界对齐。
 * 配置包全部使用显式偏移，支持不同主机字节序与 ABI。
 * 调用方负责为目标字段预留足够空间。
 * @param data 至少有两字节的输出位置。
 * @param value 主机顺序配置值。
 * @return None
 */
static void Put16(uint8_t *data, uint16_t value)
{
	data[0] = value & 0xff;
	data[1] = value >> 8;
}

/**
 * @authors Codex
 * @brief 构造参考协议的 372 字节基础设置及 204 字节 CH32 扩展。
 * @details 基础包含低 16 通道触发区，扩展包含高 16 通道触发区。
 * 即使只启用低位通道也发送扩展包，保持 U3Pro32 协议完整。
 * 禁用通道只影响掩码和数据宽度，不自动提高采样率上限。
 * mode 的 bit 12 选择流式传输，清除此位才是硬件 Buffer。
 * mode 的 bit 3 启用 FPGA 存储压缩，不改变上传端点的样本格式。
 * Buffer 的捕获计数先按 64 样本对齐，再换算到 16 样本寄存器单位。
 * 计数高低字按小端逐字段写入，避免主机结构体布局差异。
 * Stream 不采用有限硬件窗口，保留原有的最大计数配置。
 * 两种模式都保持无触发状态，触发窗口不在本次移植范围。
 * dso_count 字段沿用 reference 的逻辑模式赋值，不表示启用 DSO。
 * 低速标志按输入位吞吐量判断，与是否启用存储压缩无关。
 * @param sdi 已打开设备，启用通道已快照保存。
 * @return SR_OK 或配置握手错误。
 * @note Stream 与 Buffer 共用无触发配置；RLE 仅改变设备存储方式。
 */
static int Arm(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	struct sr_usb_dev_inst *usb = sdi->conn;
	// 字节数分别对应参考 DSL_setting 与 DSL_setting_ext32 的线上布局。
	uint8_t base[372] = { 0 }, ext[204] = { 0 };
	// 分频值无量纲；公布的采样率均可由 500 MHz 时钟整除。
	uint32_t divider = SR_MHZ(500) / devc->samplerate;
	unsigned int i = 0;
	int actual = 0;
	// 高字中的位 8 及以上保存预分频值减一，低位保存主分频高位。
	uint16_t div_h = ((MIN(divider, 5) - 1) << 8);

	// 按协议偏移逐项写入，避免 C 结构体的隐式对齐影响线上布局。
	memcpy(base, "\xa5\xf5\xa5\xf5", 4);
	Put16(base + 4, 0x0001);
	Put16(base + 6, (devc->buffer_mode ? 0 : (1 << 12)) |
		(devc->rle ? (1 << 3) : 0) |
		(((devc->samplerate * devc->channel_count / 8000) < 1024) ? (1 << 10) : 0));
	Put16(base + 8, 0x0102);
	divider = (divider + 4) / 5;
	Put16(base + 10, divider & 0xffff);
	Put16(base + 12, div_h | (divider >> 16));
	Put16(base + 14, 0x0302);
	// Buffer 按 16 样本单位编程；Stream 保留原来的硬件最大计数。
	Put16(base + 16, devc->buffer_mode ? ((devc->capture_samples >> 4) & 0xffff) : 0xffff);
	Put16(base + 18, devc->buffer_mode ? (devc->capture_samples >> 20) : 0xffff);
	Put16(base + 20, 0x0502);
	Put16(base + 22, U3_ATOMIC_SAMPLES);
	Put16(base + 26, 0x0701);
	Put16(base + 28, (devc->channel_count & 0x1f) << 8);
	Put16(base + 30, 0x0802);
	Put16(base + 32, devc->buffer_mode ? (devc->capture_samples & 0xffff) : 0xffff);
	Put16(base + 34, devc->buffer_mode ? ((devc->capture_samples >> 16) & 0xffff) : 0xffff);
	Put16(base + 36, 0x0a02);
	Put16(base + 38, devc->channel_mask & 0xffff);
	Put16(base + 40, devc->channel_mask >> 16);
	Put16(base + 42, 0x0c01);
	Put16(base + 46, 0x40a0);
	memcpy(base + 368, "\x5a\xfa\x5a\xfa", 4);
	memcpy(ext, "\xa5\xf5\xa5\xf5", 4);
	Put16(ext + 4, 0x6060);
	Put16(ext + 198, 0xffff);
	memcpy(ext + 200, "\x5a\xfa\x5a\xfa", 4);

	for (i = 0; i < 16; i++) {
		Put16(base + 48 + (i * 2), 0xffff);
		Put16(base + 80 + (i * 2), 0xffff);
		Put16(base + 240 + (i * 2), 2);
		Put16(base + 272 + (i * 2), 2);
		Put16(ext + 6 + (i * 2), 0xffff);
		Put16(ext + 38 + (i * 2), 0xffff);
	}

	if ((devc->usb_speed != LIBUSB_SPEED_SUPER) &&
		(WriteByte(sdi, U3_CTL_WORDWIDE, 1) != SR_OK))
		return SR_ERR;

	if (BulkWrite(sdi, base, sizeof(base), TRUE) != SR_OK)
		return SR_ERR;

	// CH32 扩展紧随基础包，不重复发送 BULK_WR 命令。
	if ((libusb_bulk_transfer(usb->devhdl, 2, ext, sizeof(ext),
		&actual, U3_USB_TIMEOUT) != 0) || (actual != sizeof(ext)))
		return SR_ERR;

	if (WriteByte(sdi, U3_CTL_INTRDY, 0x80) != SR_OK)
		return SR_ERR;

	return WaitStatus(sdi, 0x80);
}

/**
 * @authors Codex
 * @brief 解交织一个完整原子组，映射到 32 位物理通道输出。
 * @details 输入按启用通道的物理编号递增排列，每通道连续占八字节。
 * 每字节的低位对应较早样本，输出保持标准 libsigrok 小端位映射。
 * 最后一组可以只输出前若干采样，未输出部分不累计到会话计数。
 * @param sdi 正在采集的设备。
 * @param data channel_count * 8 字节输入，不得为 NULL。
 * @return SR_OK 或会话数据发送错误。
 * @note 使用按字节取位，支持非对齐输入及大小端主机。
 */
static int SendGroup(const struct sr_dev_inst *sdi, const uint8_t *data)
{
	struct dev_context *devc = sdi->priv;
	unsigned int bit = 0, channel = 0, packed = 0;
	uint64_t count = U3_ATOMIC_SAMPLES;
	struct sr_datafeed_logic logic = { 0 };
	struct sr_datafeed_packet packet = { 0 };

	if (!devc->continuous && devc->limit_samples)
		count = MIN(count, devc->limit_samples - devc->sent_samples);

	// 先清空输出，保证禁用通道和上一组剩余位不会污染新样本。
	memset(devc->output, 0, sizeof(devc->output));
	for (channel = 0; channel < U3_CHANNELS; channel++) {
		if (!(devc->channel_mask & (UINT32_C(1) << channel)))
			continue;

		for (bit = 0; bit < count; bit++) {
			if (data[(packed * 8) + (bit / 8)] & (1 << (bit % 8)))
				devc->output[(bit * 4) + (channel / 8)] |= 1 << (channel % 8);
		}

		// 输入仅包含启用通道，因此物理编号与紧凑输入索引分别推进。
		packed++;
	}

	// 无论实际启用多少通道，输出位号始终对应 D0 至 D31。
	logic.unitsize = 4;
	logic.length = count * 4;
	logic.data = devc->output;
	packet.type = SR_DF_LOGIC;
	packet.payload = &logic;
	// 发送失败后不能将未发布的样本计入已发送总数。
	if (count && (sr_session_send(sdi, &packet) != SR_OK))
		return SR_ERR;

	devc->sent_samples += count;
	return SR_OK;
}

/**
 * @authors Codex
 * @brief 按平台移除与本设备配对的会话事件源。
 * @details Windows 使用设备上下文作键，避免多个定时源共享 fd=-1 的键。
 * 仅在传输完成或尚未提交时调用，取消请求本身不允许移除事件源。
 * 非 Windows 保留公共 USB 源的上下文键及 pollfd 生命周期。
 * @param sdi 会话和私有上下文仍有效的设备，不得为 NULL。
 * @return None
 */
static void RemoveEvents(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;

#ifdef _WIN32
	sr_session_source_remove_internal(sdi->session, devc);
#else
	usb_source_remove(sdi->session, devc->ctx);
#endif
}

/**
 * @authors Codex
 * @brief 释放唯一异步传输并发送一次 END；取消完成后才释放资源。
 * @details 只能从传输完成回调调用，不可在 libusb 仍持有传输时提前释放。
 * 在发送 END 前标记采集结束，使上层可查询到最终状态。
 * 事件源与本次采集配对移除，不改动驱动实例和设备连接对象。
 * @param sdi 正在结束的采集设备。
 * @return None
 */
static void Finish(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;

	g_free(devc->transfer->buffer);
	devc->transfer->buffer = NULL;
	libusb_free_transfer(devc->transfer);
	devc->transfer = NULL;
	devc->running = FALSE;
	if (devc->source_added) {
		RemoveEvents(sdi);
		devc->source_added = FALSE;
	}

	std_session_send_df_end(sdi);
}

/**
 * @authors Codex
 * @brief 异步 STOP 完成后释放传输并结束会话。
 * @param transfer 本驱动持有的停止控制传输，缓冲区由 Finish 释放。
 * @return None
 */
static void LIBUSB_CALL StopComplete(struct libusb_transfer *transfer)
{
	const struct sr_dev_inst *sdi = transfer->user_data;

	if ((transfer->status != LIBUSB_TRANSFER_COMPLETED) ||
		(transfer->actual_length != 4))
		sr_err("U3Pro32 asynchronous STOP failed: status %d, length %d.",
			transfer->status, transfer->actual_length);

	Finish(sdi);
}

/**
 * @authors Codex
 * @brief 复用已完成的传输异步发送 STOP，避免回调内同步请求返回 BUSY。
 * @details 只能在采集传输完成回调中调用；提交后由 StopComplete 收尾。
 * 分配或提交失败仍释放原传输并结束会话，不遗留事件源。
 * @param sdi 当前采集设备，生命周期须保持到停止传输完成。
 * @return None
 */
static void SubmitStop(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	// USB 控制头后携带四字节 DSL 命令头，无附加负载。
	uint8_t *buffer = g_try_malloc0(LIBUSB_CONTROL_SETUP_SIZE + 4);
	int ret = LIBUSB_SUCCESS;

	devc->stopping = TRUE;
	if (!buffer) {
		sr_err("Unable to allocate U3Pro32 STOP request.");
		Finish(sdi);
		return;
	}

	libusb_fill_control_setup(buffer,
		LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT, 0xb0, 0, 0, 4);
	buffer[LIBUSB_CONTROL_SETUP_SIZE] = U3_CTL_STOP;
	// 原采集传输已完成；替换缓冲区后所有权仍由 Finish 统一回收。
	g_free(devc->transfer->buffer);
	devc->transfer->buffer = NULL;
	libusb_fill_control_transfer(devc->transfer,
		((struct sr_usb_dev_inst *)sdi->conn)->devhdl, buffer,
		StopComplete, (void *)sdi, U3_USB_TIMEOUT);
	ret = libusb_submit_transfer(devc->transfer);
	if (ret != LIBUSB_SUCCESS) {
		sr_err("Unable to submit U3Pro32 STOP: %s (%d).",
			libusb_error_name(ret), ret);
		Finish(sdi);
	}
}

/**
 * @authors Codex
 * @brief 处理头部或数据回调，拼接短 USB 包并按限制终止。
 * @details 头部必须独立占满 1024 字节，错误头不会被解释成逻辑数据。
 * 普通数据允许短传输及携带有效数据的超时，剩余字节跨回调保存。
 * 取消路径优先于数据解析，保证停止后不再发布残余数据。
 * Buffer 的头部在采样完成之后才上传，等待阶段可能多次 USB 超时。
 * 空超时期间继续派发 USB 事件，用户停止仍由原有取消流程处理。
 * 头部的剩余计数单位是每通道样本数，不是压缩字节数。
 * 用已编程的对齐计数减去剩余计数，得到允许解析的有效窗口。
 * 剩余计数超过已编程计数属于协议错误，不做无符号减法。
 * 有效窗口不足用户请求时报告错误，避免发布不完整的成功结果。
 * 每个完整原子组仍包含各启用通道的连续 64 位采样。
 * 最后一笔 USB 上传可能包含填充，填充不参与原子组拼接。
 * 有效字节边界由硬件头部确定，用户输出边界由 limit_samples 确定。
 * 数据阶段超时且没有数据时结束 Buffer，避免上传中断后永久等待。
 * 带有有效数据的超时仍先处理数据，保留 libusb 短传输语义。
 * RLE 上传使用同一个解交织器，上层始终收到未压缩的标准逻辑样本。
 * @param transfer 本驱动拥有的异步传输，回调可重提交或释放。
 * @return None
 */
static void LIBUSB_CALL Receive(struct libusb_transfer *transfer)
{
	const struct sr_dev_inst *sdi = transfer->user_data;
	struct dev_context *devc = sdi->priv;
	unsigned int offset = 0, take = 0, length = transfer->actual_length;
	uint64_t remaining = 0, available = 0;
	// 每个启用通道提供 64 位样本，组长单位为字节。
	unsigned int group_size = devc->channel_count * 8;
	gboolean valid = (transfer->status == LIBUSB_TRANSFER_COMPLETED) ||
		(transfer->status == LIBUSB_TRANSFER_TIMED_OUT);

	// 取消完成后异步发送 STOP，不重提交已经请求停止的采集。
	if (devc->stopping)
		goto finish;

	if (!valid) {
		sr_err("USB acquisition failed: status %d.", transfer->status);
		goto stop;
	}

	if (devc->header_pending) {
		// Buffer 在存储完成前没有头部；空超时仅重新排队，允许用户取消。
		if (devc->buffer_mode && (transfer->actual_length == 0) &&
			(transfer->status == LIBUSB_TRANSFER_TIMED_OUT))
			goto resubmit;

		// U3 设备固定使用 1024 字节采集头，且检查同步标记。
		if ((transfer->actual_length != U3_HEADER_BYTES) ||
			memcmp(transfer->buffer, "\x55\x55\x55\x55", 4)) {
			sr_err("Invalid U3Pro32 acquisition header.");
			goto stop;
		}

		if (devc->buffer_mode) {
			// reference ds_trigger_pos 的低/高剩余样本计数位于字节 12/16。
			remaining = (uint64_t)RL32(transfer->buffer + 12) |
				((uint64_t)RL32(transfer->buffer + 16) << 32);
			if (remaining > devc->capture_samples) {
				sr_err("Invalid U3Pro32 Buffer remaining count.");
				goto stop;
			}

			available = (devc->capture_samples - remaining) & ~UINT64_C(63);
			// 正常 Buffer 必须覆盖用户请求；不把不足的上传静默判为完整采集。
			if (available < devc->limit_samples) {
				sr_err("Incomplete U3Pro32 Buffer capture: %" PRIu64 " samples.", available);
				goto stop;
			}

			devc->expected_bytes = (available / U3_ATOMIC_SAMPLES) * group_size;
			sr_dbg("U3Pro32 Buffer header: remaining %" PRIu64 ", upload %" PRIu64 " bytes.",
				remaining, devc->expected_bytes);
		}

		// 校验成功后切换到数据阶段，不把头部内容交给解交织器。
		devc->header_pending = FALSE;
		// 使用原先分配的完整缓冲区，无须在回调中重新分配。
		transfer->length = devc->transfer_size;
		transfer->timeout = 1000;
	} else {
		// Buffer 上传结束前的空超时意味着数据不完整，避免永久等待。
		if (devc->buffer_mode && !transfer->actual_length) {
			sr_err("U3Pro32 Buffer upload ended before the requested samples.");
			goto stop;
		}

		// 设备会填充最后一笔 USB 请求；只解释头部声明的有效样本区。
		if (devc->buffer_mode)
			length = MIN(length, devc->expected_bytes - devc->received_bytes);

		devc->received_bytes += length;

		while (offset < length) {
			take = MIN(group_size - devc->partial_length,
				length - offset);
			memcpy(devc->partial + devc->partial_length,
				transfer->buffer + offset, take);
			devc->partial_length += take;
			offset += take;

			// 只有完整原子组才能确定通道边界。
			if (devc->partial_length == group_size) {
				// 上层拒收数据时立即停止，避免继续无效的硬件传输。
				if (SendGroup(sdi, devc->partial) != SR_OK)
					goto stop;
				devc->partial_length = 0;
				// 数据消费者可以在同步发布回调中请求停止。
				if (devc->stopping)
					goto finish;
			}

			// 精确到样本数终止，允许最后一组少于 64 个输出采样。
			if (!devc->continuous && devc->limit_samples &&
				(devc->sent_samples >= devc->limit_samples))
				goto stop;
		}
	}

	// 上层可能在数据回调中请求停止，此时当前传输尚未重提交。
	if (devc->stopping)
		goto finish;

resubmit:
	if (libusb_submit_transfer(transfer) == 0)
		return;

	sr_err("Unable to resubmit U3Pro32 transfer.");
stop:
finish:
	SubmitStop(sdi);
}

/**
 * @authors Codex
 * @brief 由 libsigrok USB 事件源驱动异步回调。
 * @details 超时结构为零，事件处理不在轮询回调中额外等待。
 * Windows 的会话定时源或其他平台的公共 USB 源安排唤醒。
 * 不检查 revents，定时唤醒也必须服务完成与取消事件。
 * 回调中 Finish 可销毁事件源；事件处理返回后不再访问采集状态。
 * 传入的上下文在驱动清理完成前必须保持有效。
 * @param fd 事件描述符，由公共 USB 层管理。
 * @param revents 事件位，由公共 USB 层管理。
 * @param cb_data 有效的 sr_context，所有权不转移。
 * @return TRUE，直到 Finish 移除事件源。
 */
static int ReceiveEvents(int fd, int revents, void *cb_data)
{
	struct sr_context *ctx = cb_data;
	struct timeval timeout = { 0, 0 };
	(void)fd;
	(void)revents;

	libusb_handle_events_timeout(ctx->libusb_ctx, &timeout);
	return TRUE;
}

/**
 * @authors Codex
 * @brief 注册本驱动的 USB 调度，Windows 不使用 libusb pollfd 接口。
 * @details 1 ms 是 USB 服务间隔，不是传输超时；原 USB 请求超时保持不变。
 * 会话定时源在原线程零等待派发事件，不创建 idle 空转或第二个线程。
 * GLib 使用单调时间，并在回调移除源后避免重新安排已销毁的源。
 * Windows 实际精度由系统决定；完成事件及 libusb 超时至迟在下次唤醒服务。
 * 源先于异步传输注册，保留到取消及 STOP 完成，失败时沿用启动回滚。
 * @param sdi 当前设备，会话和私有上下文必须保持到最终完成回调。
 * @return SR_OK 或会话事件源注册错误。
 */
static int AddEvents(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;

#ifdef _WIN32
	// 负 fd 只创建定时源；独立设备键避免与其他驱动的定时源冲突。
	sr_info("U3Pro32 Windows USB event fallback enabled (1 ms session timer).");
	return sr_session_fd_source_add(sdi->session, devc, -1, 0, 1,
		ReceiveEvents, devc->ctx);
#else
	return usb_source_add(sdi->session, devc->ctx, 10,
		ReceiveEvents, devc->ctx);
#endif
}

/**
 * @authors Codex
 * @brief 沿用 reference 流模式时长，按吞吐量计算 USB 数据请求长度。
 * @details SuperSpeed 取 10 ms，High Speed 取 20 ms，按端点包长向上对齐。
 * 最少容纳一个采集头，最多使用现有缓冲上限，避免低速请求长期不满。
 * @param devc 本次通道快照和采样率已确定的私有上下文，不得为 NULL。
 * @return 实际请求及缓冲分配长度，单位字节。
 */
static unsigned int TransferSize(const struct dev_context *devc)
{
	unsigned int packet_size = (devc->usb_speed == LIBUSB_SPEED_SUPER) ? 1024 : 512;
	unsigned int milliseconds = (devc->usb_speed == LIBUSB_SPEED_SUPER) ? 10 : 20;
	// 每样本每启用通道占一位；8000 将 Hz、ms 和 bit 换算为字节。
	uint64_t bytes = (devc->samplerate * devc->channel_count * milliseconds + 7999) / 8000;

	bytes = MAX(bytes, U3_HEADER_BYTES);
	bytes = ((bytes + packet_size - 1) / packet_size) * packet_size;
	return MIN(bytes, U3_BUFFER_BYTES);
}

/**
 * @authors Codex
 * @brief 启动无触发 Stream 或 Buffer 采集，失败路径释放本次资源。
 * @details 通道启用状态在启动时复制，数据解释不读取之后可能改变的列表。
 * 设备先停止上一次硬件传输，再写入本次基础及扩展设置。
 * 失败发生在传输提交前时直接释放，提交成功后交由完成回调管理。
 * Buffer 容量由型号表的 bit 容量除以启用通道数换算。
 * 普通 Buffer 不允许请求超过实际存储深度的样本。
 * RLE 逻辑上限沿用 reference 的 1024 倍容量，实际可压缩性取决于输入。
 * 压缩模式仍受 32 位、16 样本单位的硬件计数字段限制。
 * 容量检查先于加法对齐，使最大无符号输入也不会产生环绕。
 * RLE 最少采集两个原子组，输出层仍可精确返回单个样本。
 * Stream 的速率上限来自 USB 速度，不能借 Buffer 配置绕过。
 * 配置键可能无序提交，因此所有字段到齐后再执行联合校验。
 * 校验失败不会向 FPGA 提交本次 Arm，也不会创建在途 USB 请求。
 * 每次启动重置上传字节数，防止切换模式时沿用上一会话边界。
 * @param sdi 已打开的设备；会话及通道列表在采集期间保持有效。
 * @return SR_OK 或配置、内存、事件源、USB 提交错误。
 */
SR_PRIV int U3Start(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	struct sr_usb_dev_inst *usb = sdi->conn;
	GSList *l = NULL;
	struct sr_channel *channel = NULL;
	uint8_t *buffer = NULL;
	uint64_t depth = 0;

	if (!usb->devhdl || devc->running)
		return SR_ERR;

	// 未实现硬件触发时明确拒绝，避免静默采集错误的时间窗口。
	if (sr_session_trigger_get(sdi->session))
		return SR_ERR_NA;

	// 重新计算快照，防止重复采集沿用上一次的通道集合。
	devc->channel_mask = 0;
	devc->channel_count = 0;
	for (l = sdi->channels; l; l = l->next) {
		channel = l->data;
		if (channel->enabled) {
			devc->channel_mask |= UINT32_C(1) << channel->index;
			devc->channel_count++;
		}
	}

	if (!devc->channel_count)
		return SR_ERR_ARG;

	if (devc->buffer_mode) {
		// 容量以 bit 换算到每通道样本；RLE 允许 reference 的 1024 倍逻辑深度。
		depth = (U3_MEMORY_BITS / devc->channel_count) & ~UINT64_C(63);
		if (devc->rle)
			depth *= U3_RLE_FACTOR;

		depth = MIN(depth, U3_MAX_CAPTURE_SAMPLES & ~UINT64_C(63));
		if (devc->continuous || !devc->limit_samples ||
			(devc->limit_samples > depth)) {
			sr_err("U3Pro32 Buffer sample limit must be 1..%" PRIu64 ".", depth);
			return SR_ERR_ARG;
		}

		// 先检查上限再向上对齐，避免 UINT64_MAX 加法溢出。
		devc->capture_samples = (devc->limit_samples + 63) & ~UINT64_C(63);
		// 实机 RLE 只有一个原子组时不产生采集头；至少采两组后精确裁剪输出。
		if (devc->rle)
			devc->capture_samples = MAX(devc->capture_samples, 2 * U3_ATOMIC_SAMPLES);
	}

	// 所有配置到齐后统一校验，拒绝 Stream 的 Buffer 专属设置。
	if (!devc->buffer_mode && (devc->rle || (devc->samplerate > devc->max_samplerate))) {
		sr_err("U3Pro32 Stream requires RLE off and samplerate <= %" PRIu64 " Hz.",
			devc->max_samplerate);
		return SR_ERR_ARG;
	}

	if ((U3Write(sdi, U3_CTL_STOP, 0, NULL, 0) != SR_OK) ||
		(Arm(sdi) != SR_OK))
		return SR_ERR;

	// 低速单通道采用短请求，避免固定大请求在 USB/IP 取消时没有有效数据。
	devc->transfer_size = TransferSize(devc);
	sr_dbg("U3Pro32 data request %u bytes, %u channels, continuous %d.",
		devc->transfer_size, devc->channel_count, devc->continuous);
	buffer = g_try_malloc(devc->transfer_size);
	if (!buffer)
		return SR_ERR_MALLOC;

	devc->transfer = libusb_alloc_transfer(0);
	if (!devc->transfer) {
		g_free(buffer);
		buffer = NULL;
		return SR_ERR_MALLOC;
	}

	// 计数和残片均按采集重置，不允许跨会话拼接。
	devc->sent_samples = 0;
	devc->received_bytes = 0;
	devc->expected_bytes = 0;
	devc->partial_length = 0;
	devc->stopping = FALSE;
	devc->header_pending = TRUE;
	libusb_fill_bulk_transfer(devc->transfer, usb->devhdl, 0x86,
		buffer, U3_HEADER_BYTES, Receive, (void *)sdi, U3_USB_TIMEOUT);

	if (AddEvents(sdi) != SR_OK)
		goto fail;

	devc->source_added = TRUE;
	// START 在提交前发送；硬件端点缓存头部，失败时无悬挂的异步回调。
	if (U3Write(sdi, U3_CTL_START, 0, NULL, 0) != SR_OK)
		goto fail;

	if (libusb_submit_transfer(devc->transfer) != 0) {
		U3Write(sdi, U3_CTL_STOP, 0, NULL, 0);
		goto fail;
	}

	devc->running = TRUE;
	// 已提交的传输只能异步取消；头部发布失败也沿用该收尾路径。
	if (std_session_send_df_header(sdi) != SR_OK) {
		U3Stop((struct sr_dev_inst *)sdi);
		return SR_ERR;
	}

	return SR_OK;
fail:
	if (devc->source_added) {
		RemoveEvents(sdi);
		devc->source_added = FALSE;
	}

	libusb_free_transfer(devc->transfer);
	devc->transfer = NULL;
	g_free(buffer);
	buffer = NULL;
	return SR_ERR;
}

/**
 * @authors Codex
 * @brief 取消采集传输，完成回调再异步停止硬件并发送 END。
 * @details 设置 stopping 后重复请求不会重复取消或重复发送 END。
 * 停止控制请求由取消完成回调提交，避免同步请求嵌套事件处理。
 * 调用方不能将取消请求返回当作传输缓冲区已经可以释放。
 * @param sdi 当前设备，不得为 NULL。
 * @return 取消提交结果；传输已完成等待回调时也返回 SR_OK。
 */
SR_PRIV int U3Stop(struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	int ret = SR_OK;

	if (!devc->running || devc->stopping)
		return SR_OK;

	devc->stopping = TRUE;
	// 不在这里释放内存；即使取消失败也由待处理完成事件收尾。
	ret = libusb_cancel_transfer(devc->transfer);
	if ((ret != LIBUSB_SUCCESS) && (ret != LIBUSB_ERROR_NOT_FOUND)) {
		sr_err("Unable to cancel U3Pro32 acquisition: %s (%d).",
			libusb_error_name(ret), ret);
		return SR_ERR;
	}

	return SR_OK;
}
