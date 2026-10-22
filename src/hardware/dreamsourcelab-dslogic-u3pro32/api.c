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
#include <math.h>
#include <stdio.h>
#include <string.h>

// 扫描只提供标准 USB 连接过滤，不引入其他型号配置。
static const uint32_t scanopts[] = { SR_CONF_CONN };
// U3Pro32 在本驱动中仅作为数字逻辑分析仪公开。
static const uint32_t drvopts[] = { SR_CONF_LOGIC_ANALYZER };
// 每项能力标志必须与对应 get、set、list 实现保持一致。
static const uint32_t devopts[] = {
	SR_CONF_CONN | SR_CONF_GET,
	SR_CONF_SAMPLERATE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
	SR_CONF_LIMIT_SAMPLES | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_CONTINUOUS | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_DEVICE_MODE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
	SR_CONF_RLE | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_VOLTAGE_THRESHOLD | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
};
// 共用 500 MHz / 5 预分频；Buffer 32 通道可达 250 MHz，不包含通道复用档。
static const uint64_t samplerates[] = {
	SR_KHZ(100), SR_KHZ(200), SR_KHZ(400), SR_KHZ(500), SR_MHZ(1), SR_MHZ(2),
	SR_MHZ(4), SR_MHZ(5), SR_MHZ(10), SR_MHZ(20), SR_MHZ(25), SR_MHZ(50),
	SR_MHZ(100), SR_MHZ(125), SR_MHZ(250),
};
// 配置值保持固定英文，供标准 sigrok-cli 和其他 libsigrok 前端使用。
static const char *device_modes[] = { "stream", "buffer" };

/**
 * @authors Codex
 * @brief 枚举唯一支持的 VID/PID，保留旧驱动的标准 USB 扫描方式。
 * @details 连接过滤使用公共 sr_usb_find，支持调用方指定 USB 地址。
 * 扫描仅查询描述符，不写固件，也不改变其他型号的状态。
 * 扫描结果通过 std_scan_complete 交给标准设备生命周期管理。
 * 高速和超高速连接采用不同的全通道采样率上限。
 * @param di 已初始化的驱动实例，不得为 NULL。
 * @param options 可选的连接过滤列表，所有权属于调用方。
 * @return 标准扫描结果；设备与私有状态由 libsigrok 管理。
 */
static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
	struct drv_context *drvc = di->context;
	GSList *devices = NULL, *connections = NULL, *l = NULL;
	struct sr_config *option = NULL;
	libusb_device **list = NULL;
	struct libusb_device_descriptor des = { 0 };
	struct sr_dev_inst *sdi = NULL;
	struct dev_context *devc = NULL;
	struct sr_usb_dev_inst *usb = NULL;
	struct sr_channel_group *group = NULL;
	struct sr_channel *channel = NULL;
	const char *conn = NULL;
	char path[64] = { 0 }, name[16] = { 0 };
	ssize_t count = 0, i = 0;
	int j = 0, speed = 0;
	gboolean matched = FALSE;

	for (l = options; l; l = l->next) {
		option = l->data;
		if (option->key == SR_CONF_CONN)
			conn = g_variant_get_string(option->data, NULL);
	}

	// 指定过滤条件但未找到连接时，不退化为全设备扫描。
	if (conn)
		connections = sr_usb_find(drvc->sr_ctx->libusb_ctx, conn);

	// libusb 返回负值时列表不保证有效，因此只释放过滤列表。
	count = libusb_get_device_list(drvc->sr_ctx->libusb_ctx, &list);
	if (count < 0)
		goto done;

	for (i = 0; i < count; i++) {
		if (libusb_get_device_descriptor(list[i], &des) < 0)
			continue;

		// 只匹配 U3Pro32；绝不接管 FX2 或其他 DSL 产品。
		if ((des.idVendor != 0x2a0e) || (des.idProduct != 0x002c))
			continue;

		// 没有过滤选项时接受本型号的所有有效连接。
		matched = !conn;
		for (l = connections; l; l = l->next) {
			usb = l->data;
			if ((usb->bus == libusb_get_bus_number(list[i])) &&
				(usb->address == libusb_get_device_address(list[i])))
				matched = TRUE;
		}

		if (!matched)
			continue;

		// 参考配置只定义 USB High Speed 和 SuperSpeed 两种链路。
		speed = libusb_get_device_speed(list[i]);
		if ((speed != LIBUSB_SPEED_HIGH) && (speed != LIBUSB_SPEED_SUPER))
			continue;

		// 物理路径供上层区分多台同型号设备，不包含开发机路径。
		if (usb_get_port_path(list[i], path, sizeof(path)) < 0)
			continue;

		// GLib 的 g_malloc0 分配失败会终止进程，不会返回可误用的 NULL。
		sdi = g_malloc0(sizeof(*sdi));
		devc = g_malloc0(sizeof(*devc));
		sdi->priv = devc;
		sdi->vendor = g_strdup("DreamSourceLab");
		sdi->model = g_strdup("DSLogic U3Pro32");
		sdi->connection_id = g_strdup(path);
		// 扫描不打开接口，直到用户请求 dev_open 才访问硬件。
		sdi->status = SR_ST_INACTIVE;
		sdi->inst_type = SR_INST_USB;
		sdi->conn = sr_usb_dev_inst_new(libusb_get_bus_number(list[i]),
			libusb_get_device_address(list[i]), NULL);
		// 公共上下文由库持有，私有状态只保存借用指针。
		devc->ctx = drvc->sr_ctx;
		devc->usb_speed = speed;
		// 保守地按全通道带宽限制，禁用通道不自动解锁更高速率。
		devc->max_samplerate = (speed == LIBUSB_SPEED_SUPER) ? SR_MHZ(50) : SR_MHZ(10);
		devc->samplerate = SR_MHZ(1);
		devc->limit_samples = SR_MHZ(1);
		// 默认持续采集；未显式请求连续时，设置样本上限会进入有限模式。
		devc->continuous = TRUE;
		devc->threshold = 1.0;
		// 全部 32 个数字通道归入同一个 Logic 组。
		group = sr_channel_group_new(sdi, "Logic", NULL);

		for (j = 0; j < U3_CHANNELS; j++) {
			snprintf(name, sizeof(name), "%d", j);
			channel = sr_channel_new(sdi, j, SR_CHANNEL_LOGIC, TRUE, name);
			group->channels = g_slist_append(group->channels, channel);
		}

		// 设备实例及通道最终由标准 dev_clear 释放。
		devices = g_slist_append(devices, sdi);
	}

	// 丢弃本次枚举引用；连接对象保存总线与地址而不持有列表。
	libusb_free_device_list(list, 1);
done:
	g_slist_free_full(connections, (GDestroyNotify)sr_usb_dev_inst_free);
	return std_scan_complete(di, devices);
}

/**
 * @authors Codex
 * @brief 停止采集并等待取消回调完成，再关闭 USB 句柄。
 * @details libusb 的取消调用不会同步释放传输，必须继续派发完成事件。
 * 设备状态和会话必须在取消完成前保持有效。
 * 关闭路径与正常停止共用 U3Stop，避免重复发送结束包。
 * 无句柄时允许重复关闭，不访问已释放的 USB 连接。
 * @param sdi 有效的设备实例，连接所有权保持不变。
 * @return SR_OK；取消期间保留设备及其传输缓冲区。
 */
static int dev_close(struct sr_dev_inst *sdi)
{
	struct sr_usb_dev_inst *usb = sdi->conn;
	struct dev_context *devc = sdi->priv;
	struct timeval timeout = { 0, 10000 };

	if (devc->running) {
		U3Stop(sdi);
		// libusb 取消是异步操作，清理设备前必须回收完成回调。
		while (devc->running)
			libusb_handle_events_timeout(devc->ctx->libusb_ctx, &timeout);
	}

	// 仅在取消回调完成后释放接口，避免传输访问失效句柄。
	if (usb->devhdl) {
		libusb_release_interface(usb->devhdl, 0);
		libusb_close(usb->devhdl);
		usb->devhdl = NULL;
	}

	return SR_OK;
}

/**
 * @authors Codex
 * @brief 读取当前配置；阈值使用标准 libsigrok 双精度二元组。
 * @details 配置读取不产生 USB 请求，返回驱动维护的最近有效值。
 * 采样率以 Hz 返回，采样限制以每通道样本数返回。
 * 连接地址来自扫描结果，不缓存可能变化的设备路径字符串。
 * 每次返回新建的 GVariant，遵循 libsigrok 配置接口的引用规则。
 * @param key 配置键。
 * @param data 接收新建 GVariant，不得为 NULL。
 * @param sdi 设备实例，不得为 NULL。
 * @param cg 可选通道组，当前配置作用于整机。
 * @return SR_OK、SR_ERR_ARG 或 SR_ERR_NA。
 */
static int config_get(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc = sdi ? sdi->priv : NULL;
	struct sr_usb_dev_inst *usb = sdi ? sdi->conn : NULL;
	(void)cg;

	if (!devc || !data)
		return SR_ERR_ARG;

	switch (key) {
	case SR_CONF_CONN:
		// 使用 libsigrok 通用的 bus.address 表达式。
		*data = g_variant_new_printf("%d.%d", usb->bus, usb->address);
		break;
	case SR_CONF_SAMPLERATE:
		*data = g_variant_new_uint64(devc->samplerate);
		break;
	case SR_CONF_LIMIT_SAMPLES:
		*data = g_variant_new_uint64(devc->limit_samples);
		break;
	case SR_CONF_DEVICE_MODE:
		*data = g_variant_new_string(device_modes[devc->buffer_mode ? 1 : 0]);
		break;
	case SR_CONF_RLE:
		*data = g_variant_new_boolean(devc->rle);
		break;
	case SR_CONF_CONTINUOUS:
		*data = g_variant_new_boolean(devc->continuous);
		break;
	case SR_CONF_VOLTAGE_THRESHOLD:
		*data = std_gvar_tuple_double(devc->threshold, devc->threshold);
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

/**
 * @authors Codex
 * @brief 校验配置，采集期间禁止更改硬件或样本解释方式。
 * @details 单项校验取值；模式、速率与 RLE 的联合约束在启动时检查。
 * 标准 CLI 不保证配置键应用顺序，因此允许暂存尚未配套的有效取值。
 * 显式 continuous=true 优先于有限上限缓存，消除 CLI 键应用顺序差异。
 * 要从显式连续切回有限模式，调用方须先设置 continuous=false。
 * 默认连续仍允许仅设置样本上限切回有限采集，保留常用 CLI 行为。
 * 设备尚未打开时允许保存阈值，打开时统一写入硬件。
 * 阈值元组的两个端点必须代表同一比较电平，不接受区间模式。
 * @param key 配置键。
 * @param data 类型由配置键决定的 GVariant，不得为 NULL。
 * @param sdi 设备实例，不得为 NULL。
 * @param cg 可选通道组，配置作用于整机。
 * @return SR_OK 或参数、USB 写入错误。
 */
static int config_set(uint32_t key, GVariant *data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc = sdi ? sdi->priv : NULL;
	struct sr_usb_dev_inst *usb = sdi ? sdi->conn : NULL;
	int idx = 0, ret = SR_OK;
	double low = 0.0, high = 0.0;
	(void)cg;

	if (!devc || !data || devc->running)
		return SR_ERR_ARG;

	switch (key) {
	case SR_CONF_SAMPLERATE:
		idx = std_u64_idx(data, ARRAY_AND_SIZE(samplerates));
		if ((idx < 0) || (samplerates[idx] > SR_MHZ(250)))
			return SR_ERR_ARG;

		// 校验成功后才更新缓存，错误输入不影响旧设置。
		devc->samplerate = samplerates[idx];
		break;
	case SR_CONF_LIMIT_SAMPLES:
		devc->limit_samples = g_variant_get_uint64(data);
		// 缓存有限上限；显式连续意图不能被 CLI 后设置的样本键覆盖。
		// 未显式请求连续时，设置样本数仍按原有行为进入有限采集。
		devc->continuous = devc->continuous_requested;
		break;
	case SR_CONF_DEVICE_MODE:
		idx = std_str_idx(data, ARRAY_AND_SIZE(device_modes));
		if ((idx < 0) || ((idx == 1) && devc->continuous_requested))
			return SR_ERR_ARG;

		// Buffer 使用已有有限上限；返回 Stream 时不隐式恢复连续采集。
		devc->buffer_mode = (idx == 1);
		if (devc->buffer_mode)
			devc->continuous = FALSE;

		// 速率和 RLE 保持原值，联合约束在启动时检查。
		break;
	case SR_CONF_RLE:
		// 与 device_mode 的联合合法性延后检查，兼容 CLI 无序设置。
		devc->rle = g_variant_get_boolean(data);
		break;
	case SR_CONF_CONTINUOUS:
		// 硬件 Buffer 不能无限采集；不能把 continuous 当成硬件模式选择。
		if (devc->buffer_mode && g_variant_get_boolean(data))
			return SR_ERR_ARG;

		devc->continuous = g_variant_get_boolean(data);
		devc->continuous_requested = devc->continuous;
		break;
	case SR_CONF_VOLTAGE_THRESHOLD:
		g_variant_get(data, "(dd)", &low, &high);
		// 相同阈值二元组代表单一可调比较电平；拒绝 NaN 与倒置区间。
		if (!isfinite(low) || !isfinite(high) || (low < 0.0) ||
			(high > 5.0) || (fabs(high - low) > 0.000001))
			return SR_ERR_ARG;

		// 打开前仅缓存；打开后先写入硬件再提交缓存值。
		if (usb->devhdl)
			ret = U3Threshold(sdi, low);

		if (ret == SR_OK)
			devc->threshold = low;
		return ret;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

/**
 * @authors Codex
 * @brief 按实际 USB 速度公布已实现的配置能力。
 * @details 驱动级查询不要求设备实例，具体采样率查询要求有效私有上下文。
 * Stream 按 USB 速度限制，Buffer 采用独立的 250 MHz 全通道上限。
 * 模式列表不包含尚未实现的通道复用及内部测试选项。
 * 阈值范围采用标准最小值、最大值、步长描述，单位为 V。
 * @param key 配置键。
 * @param data 输出 GVariant，不得为 NULL。
 * @param sdi 可选设备实例，查询具体采样率时必需。
 * @param cg 可选通道组。
 * @return SR_OK 或不支持、参数错误。
 */
static int config_list(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc = sdi ? sdi->priv : NULL;
	size_t count = 0;

	switch (key) {
	case SR_CONF_SCAN_OPTIONS:
	case SR_CONF_DEVICE_OPTIONS:
		return STD_CONFIG_LIST(key, data, sdi, cg, scanopts, drvopts, devopts);
	case SR_CONF_DEVICE_MODE:
		*data = g_variant_new_strv(device_modes, ARRAY_SIZE(device_modes));
		break;
	case SR_CONF_SAMPLERATE:
		if (!devc)
			return SR_ERR_ARG;

		// 保持升序列表，并保留上下限内的全部已验证整数分频值。
		while ((count < ARRAY_SIZE(samplerates)) &&
			(samplerates[count] <= (devc->buffer_mode ? SR_MHZ(250) : devc->max_samplerate)))
			count++;

		*data = std_gvar_samplerates(samplerates, count);
		break;
	case SR_CONF_VOLTAGE_THRESHOLD:
		// 比较器可调范围为 0 至 5 V，界面步进为 0.1 V。
		*data = std_gvar_min_max_step_thresholds(0.0, 5.0, 0.1);
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

// 复用库的驱动生命周期，只替换本型号需要的硬件操作。
static struct sr_dev_driver dreamsourcelab_dslogic_u3pro32_driver_info = {
	.name = "dreamsourcelab-dslogic-u3pro32",
	.longname = "DreamSourceLab DSLogic U3Pro32",
	.api_version = 1,
	.init = std_init,
	.cleanup = std_cleanup,
	.scan = scan,
	.dev_list = std_dev_list,
	.dev_clear = std_dev_clear,
	.config_get = config_get,
	.config_set = config_set,
	.config_list = config_list,
	.dev_open = U3Open,
	.dev_close = dev_close,
	.dev_acquisition_start = U3Start,
	.dev_acquisition_stop = U3Stop,
	.context = NULL,
};
SR_REGISTER_DEV_DRIVER(dreamsourcelab_dslogic_u3pro32_driver_info);
