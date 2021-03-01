/*
 * Copyright (c) 2019 Vestas Wind Systems A/S
 *
 * Based on adc_mcux_rt_adc.c, which is:
 * Copyright (c) 2017-2018, NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT nxp_mcux_rtadc

#include <drivers/adc.h>
#include <fsl_adc.h>

#define LOG_LEVEL CONFIG_ADC_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(adc_mcux_rt);

#define ADC_CONTEXT_USES_KERNEL_TIMER
#include "adc_context.h"

struct mcux_rt_adc_config {
	ADC_Type *base;
	adc_clock_source_t clock_src;
	adc_clock_driver_t clock_drv;
	adc_reference_voltage_source_t ref_src;
	adc_sample_period_mode_t sample_period_mode;
	void (*irq_config_func)(const struct device *dev);
};

struct mcux_rt_adc_data {
	const struct device *dev;
	struct adc_context ctx;
	uint16_t *buffer;
	uint16_t *repeat_buffer;
	uint32_t channels;
	uint8_t channel_id;
};

static int mcux_rt_adc_channel_setup(const struct device *dev,
				     const struct adc_channel_cfg *channel_cfg)
{
	uint8_t channel_id = channel_cfg->channel_id;

	if (channel_id > (ADC_HC_ADCH_MASK >> ADC_HC_ADCH_SHIFT)) {
		LOG_ERR("Invalid channel %d", channel_id);
		return -EINVAL;
	}

	if (channel_cfg->acquisition_time != ADC_ACQ_TIME_DEFAULT) {
		LOG_ERR("Unsupported channel acquisition time");
		return -ENOTSUP;
	}

	if (channel_cfg->differential) {
		LOG_ERR("Differential channels are not supported");
		return -ENOTSUP;
	}

	if (channel_cfg->gain != ADC_GAIN_1) {
		LOG_ERR("Unsupported channel gain %d", channel_cfg->gain);
		return -ENOTSUP;
	}

	if (channel_cfg->reference != ADC_REF_INTERNAL) {
		LOG_ERR("Unsupported channel reference");
		return -ENOTSUP;
	}

	return 0;
}

static int mcux_rt_adc_start_read(const struct device *dev,
				  const struct adc_sequence *sequence)
{
	const struct mcux_rt_adc_config *config = dev->config;
	struct mcux_rt_adc_data *data = dev->data;
	adc_hardware_average_mode_t mode;
	adc_resolution_t resolution;
	ADC_Type *base = config->base;
	int error;
	uint32_t tmp32;

	switch (sequence->resolution) {
	case 8:
		resolution = kADC_Resolution8Bit;
		break;
	case 10:
		resolution = kADC_Resolution10Bit;
		break;
	case 12:
		resolution = kADC_Resolution12Bit;
		break;
	default:
		LOG_ERR("Unsupported resolution %d", sequence->resolution);
		return -ENOTSUP;
	}

	tmp32 = base->CFG & ~(ADC_CFG_MODE_MASK);
	tmp32 |= ADC_CFG_MODE(resolution);
	base->CFG = tmp32;

	switch (sequence->oversampling) {
	case 0:
		mode = kADC_HardwareAverageDiasable;
		break;
	case 2:
		mode = kADC_HardwareAverageCount4;
		break;
	case 3:
		mode = kADC_HardwareAverageCount8;
		break;
	case 4:
		mode = kADC_HardwareAverageCount16;
		break;
	case 5:
		mode = kADC_HardwareAverageCount32;
		break;
	default:
		LOG_ERR("Unsupported oversampling value %d",
			sequence->oversampling);
		return -ENOTSUP;
	}
	ADC_SetHardwareAverageConfig(config->base, mode);

	data->buffer = sequence->buffer;
	adc_context_start_read(&data->ctx, sequence);
	error = adc_context_wait_for_completion(&data->ctx);

	return error;
}

static int mcux_rt_adc_read_async(const struct device *dev,
				  const struct adc_sequence *sequence,
				  struct k_poll_signal *async)
{
	struct mcux_rt_adc_data *data = dev->data;
	int error;

	adc_context_lock(&data->ctx, async ? true : false, async);
	error = mcux_rt_adc_start_read(dev, sequence);
	adc_context_release(&data->ctx, error);

	return error;
}

static int mcux_rt_adc_read(const struct device *dev,
			    const struct adc_sequence *sequence)
{
	return mcux_rt_adc_read_async(dev, sequence, NULL);
}

static void mcux_rt_adc_start_channel(const struct device *dev)
{
	const struct mcux_rt_adc_config *config = dev->config;
	struct mcux_rt_adc_data *data = dev->data;

	adc_channel_config_t channel_config;
	uint32_t channel_group = 0U;

	data->channel_id = find_lsb_set(data->channels) - 1;

	LOG_DBG("Starting channel %d", data->channel_id);
	channel_config.enableInterruptOnConversionCompleted = true;
	channel_config.channelNumber = data->channel_id;
	ADC_SetChannelConfig(config->base, channel_group, &channel_config);
}

static void adc_context_start_sampling(struct adc_context *ctx)
{
	struct mcux_rt_adc_data *data =
		CONTAINER_OF(ctx, struct mcux_rt_adc_data, ctx);

	data->channels = ctx->sequence.channels;
	data->repeat_buffer = data->buffer;

	mcux_rt_adc_start_channel(data->dev);
}

static void adc_context_update_buffer_pointer(struct adc_context *ctx,
					      bool repeat_sampling)
{
	struct mcux_rt_adc_data *data =
		CONTAINER_OF(ctx, struct mcux_rt_adc_data, ctx);

	if (repeat_sampling) {
		data->buffer = data->repeat_buffer;
	}
}

static void mcux_rt_adc_isr(const struct device *dev)
{
	const struct mcux_rt_adc_config *config = dev->config;
	struct mcux_rt_adc_data *data = dev->data;
	ADC_Type *base = config->base;
	uint32_t channel_group = 0U;
	uint16_t result;

	result = ADC_GetChannelConversionValue(base, channel_group);
	LOG_DBG("Finished channel %d. Result is 0x%04x", data->channel_id,
		result);

	*data->buffer++ = result;
	data->channels &= ~BIT(data->channel_id);

	if (data->channels) {
		mcux_rt_adc_start_channel(dev);
	} else {
		adc_context_on_sampling_done(&data->ctx, dev);
	}
}

static int mcux_rt_adc_init(const struct device *dev)
{
	const struct mcux_rt_adc_config *config = dev->config;
	struct mcux_rt_adc_data *data = dev->data;
	ADC_Type *base = config->base;
	adc_config_t adc_config;

	ADC_GetDefaultConfig(&adc_config);

	adc_config.referenceVoltageSource = config->ref_src;
	adc_config.clockSource = config->clock_src;
	adc_config.clockDriver = config->clock_drv;
	adc_config.samplePeriodMode = config->sample_period_mode;
	adc_config.resolution = kADC_Resolution12Bit;
	adc_config.enableContinuousConversion = false;
	adc_config.enableOverWrite = false;
	adc_config.enableHighSpeed = false;
	adc_config.enableLowPower = false;
	adc_config.enableLongSample = false;
	adc_config.enableAsynchronousClockOutput = true;

	ADC_Init(base, &adc_config);
	ADC_DoAutoCalibration(base);
	ADC_EnableHardwareTrigger(base, false);

	config->irq_config_func(dev);
	data->dev = dev;

	adc_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static const struct adc_driver_api mcux_rt_adc_driver_api = {
	.channel_setup = mcux_rt_adc_channel_setup,
	.read = mcux_rt_adc_read,
#ifdef CONFIG_ADC_ASYNC
	.read_async = mcux_rt_adc_read_async,
#endif
};

#define ASSERT_RT_ADC_CLK_DIV_VALID(val, str)                                  \
	BUILD_ASSERT(val == 1 || val == 2 || val == 4 || val == 8, str)
#define TO_RT_ADC_CLOCK_DIV(val) _DO_CONCAT(kADC_ClockDriver, val)

#define ACD_MCUX_RT_INIT(n)                                                    \
	static void mcux_rt_adc_config_func_##n(const struct device *dev);     \
                                                                               \
	ASSERT_RT_ADC_CLK_DIV_VALID(DT_INST_PROP(n, clk_divider),              \
				    "Invalid clock divider");                  \
                                                                               \
	static const struct mcux_rt_adc_config mcux_rt_adc_config_##n = {      \
		.base = (ADC_Type *)DT_INST_REG_ADDR(n),                       \
		.clock_src = kADC_ClockSourceAD,                              \
		.clock_drv = TO_RT_ADC_CLOCK_DIV(DT_INST_PROP(n, clk_divider)),\
		.ref_src = kADC_ReferenceVoltageSourceAlt0,                    \
		.sample_period_mode = kADC_SamplePeriod2or12Clocks,            \
		.irq_config_func = mcux_rt_adc_config_func_##n,                \
	};                                                                     \
                                                                               \
	static struct mcux_rt_adc_data mcux_rt_adc_data_##n = {                \
		ADC_CONTEXT_INIT_TIMER(mcux_rt_adc_data_##n, ctx),             \
		ADC_CONTEXT_INIT_LOCK(mcux_rt_adc_data_##n, ctx),              \
		ADC_CONTEXT_INIT_SYNC(mcux_rt_adc_data_##n, ctx),              \
	};                                                                     \
                                                                               \
	DEVICE_DT_INST_DEFINE(n, &mcux_rt_adc_init, device_pm_control_nop,     \
			      &mcux_rt_adc_data_##n, &mcux_rt_adc_config_##n,  \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, \
			      &mcux_rt_adc_driver_api);                        \
                                                                               \
	static void mcux_rt_adc_config_func_##n(const struct device *dev)      \
	{                                                                      \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority),         \
			    mcux_rt_adc_isr, DEVICE_DT_INST_GET(n), 0);        \
                                                                               \
		irq_enable(DT_INST_IRQN(n));                                   \
	}

DT_INST_FOREACH_STATUS_OKAY(ACD_MCUX_RT_INIT)
