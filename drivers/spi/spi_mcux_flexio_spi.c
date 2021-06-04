/*
 * Copyright (c) 2021, STRIM, ALC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT nxp_imx_flexio_spi

#include <errno.h>
#include <drivers/spi.h>
#include <drivers/clock_control.h>
#include <fsl_flexio_spi.h>

#define LOG_LEVEL CONFIG_SPI_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(spi_mcux_flexio_spi);

#include "spi_context.h"



struct spi_mcux_config {
	FLEXIO_SPI_Type flexio_spi;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	void (*irq_config_func)(const struct device *dev);
};

struct spi_mcux_data {
	const struct device *dev;
	flexio_spi_master_handle_t handle;
	struct spi_context ctx;
	size_t transfer_len;
  uint8_t transfer_flags;
};


static void spi_mcux_transfer_next_packet(const struct device *dev)
{
	const struct spi_mcux_config *config = dev->config;
	struct spi_mcux_data *data = dev->data;
	FLEXIO_SPI_Type *flexio_spi = (FLEXIO_SPI_Type *)&config->flexio_spi;
	struct spi_context *ctx = &data->ctx;
	flexio_spi_transfer_t transfer;
	status_t status;

	if ((ctx->tx_len == 0) && (ctx->rx_len == 0)) {
		/* nothing left to rx or tx, we're done! */
		spi_context_cs_control(&data->ctx, false);
		spi_context_complete(&data->ctx, 0);
		return;
	}

  transfer.flags = data->transfer_flags;

	if (ctx->tx_len == 0) {
		/* rx only, nothing to tx */
		transfer.txData = NULL;
		transfer.rxData = ctx->rx_buf;
		transfer.dataSize = ctx->rx_len;
	} else if (ctx->rx_len == 0) {
		/* tx only, nothing to rx */
		transfer.txData = (uint8_t *) ctx->tx_buf;
		transfer.rxData = NULL;
		transfer.dataSize = ctx->tx_len;
	} else if (ctx->tx_len == ctx->rx_len) {
		/* rx and tx are the same length */
		transfer.txData = (uint8_t *) ctx->tx_buf;
		transfer.rxData = ctx->rx_buf;
		transfer.dataSize = ctx->tx_len;
	} else if (ctx->tx_len > ctx->rx_len) {
		/* Break up the tx into multiple transfers so we don't have to
		 * rx into a longer intermediate buffer. Leave chip select
		 * active between transfers.
		 */
		transfer.txData = (uint8_t *) ctx->tx_buf;
		transfer.rxData = ctx->rx_buf;
		transfer.dataSize = ctx->rx_len;
	} else {
		/* Break up the rx into multiple transfers so we don't have to
		 * tx from a longer intermediate buffer. Leave chip select
		 * active between transfers.
		 */
		transfer.txData = (uint8_t *) ctx->tx_buf;
		transfer.rxData = ctx->rx_buf;
		transfer.dataSize = ctx->tx_len;
	}

	data->transfer_len = transfer.dataSize;

	status = FLEXIO_SPI_MasterTransferNonBlocking(flexio_spi, &data->handle,
						 &transfer);
	if (status != kStatus_Success) {
		LOG_ERR("Transfer could not start");
	}
}


static void spi_mcux_isr(const struct device *dev)
{
	const struct spi_mcux_config *config = dev->config;
	struct spi_mcux_data *data = dev->data;
	FLEXIO_SPI_Type *flexio_spi = (FLEXIO_SPI_Type *)&config->flexio_spi;

	FLEXIO_SPI_MasterTransferHandleIRQ(flexio_spi, &data->handle);
}


static void spi_mcux_master_transfer_callback(FLEXIO_SPI_Type *flexio_spi,
		flexio_spi_master_handle_t *handle, status_t status, void *userData)
{
	struct spi_mcux_data *data = userData;

	spi_context_update_tx(&data->ctx, 1, data->transfer_len);
	spi_context_update_rx(&data->ctx, 1, data->transfer_len);

	spi_mcux_transfer_next_packet(data->dev);
}


#define USE_FSL_HAL_FOR_INIT (0)

#if (USE_FSL_HAL_FOR_INIT == 0)
static void spi_flexio_master_init(FLEXIO_SPI_Type *base, 
																	 uint8_t pol, uint8_t pha, flexio_spi_data_bitcount_mode_t mode,
																	 uint32_t src_clock, uint32_t baudrate)
{
	flexio_shifter_config_t shifterConfig;
  flexio_timer_config_t timerConfig;
	uint32_t ctrlReg  = 0;
	uint16_t timerDiv = 0;
	uint16_t timerCmp = 0;
	
	/* Clear the shifterConfig & timerConfig struct. */
	(void)memset(&shifterConfig, 0, sizeof(shifterConfig));
	(void)memset(&timerConfig, 0, sizeof(timerConfig));

#if !(defined(FSL_SDK_DISABLE_DRIVER_CLOCK_CONTROL) && FSL_SDK_DISABLE_DRIVER_CLOCK_CONTROL)
	/* Ungate flexio clock. */
	CLOCK_EnableClock(s_flexioClocks[FLEXIO_GetInstance(base->flexioBase)]);
#endif /* FSL_SDK_DISABLE_DRIVER_CLOCK_CONTROL */

	/* Configure FLEXIO SPI Master */
	ctrlReg = base->flexioBase->CTRL;
	ctrlReg &= ~(FLEXIO_CTRL_DOZEN_MASK | FLEXIO_CTRL_DBGE_MASK | FLEXIO_CTRL_FASTACC_MASK | FLEXIO_CTRL_FLEXEN_MASK);
	ctrlReg |= (FLEXIO_CTRL_DBGE(true) | FLEXIO_CTRL_FASTACC(false) | FLEXIO_CTRL_FLEXEN(true));
	ctrlReg |= FLEXIO_CTRL_DOZEN_MASK;
	base->flexioBase->CTRL = ctrlReg;

	/* Do hardware configuration. */
	/* 1. Configure the shifter 0 for tx. */
	shifterConfig.timerSelect = base->timerIndex[0];
	shifterConfig.pinConfig   = kFLEXIO_PinConfigOutput;
	shifterConfig.pinSelect   = base->SDOPinIndex;
	shifterConfig.pinPolarity = kFLEXIO_PinActiveHigh;
	shifterConfig.shifterMode = kFLEXIO_ShifterModeTransmit;
	shifterConfig.inputSource = kFLEXIO_ShifterInputFromPin;
	if (pha)
	{
		shifterConfig.timerPolarity = kFLEXIO_ShifterTimerPolarityOnPositive;
		shifterConfig.shifterStop   = kFLEXIO_ShifterStopBitLow;
		shifterConfig.shifterStart  = kFLEXIO_ShifterStartBitDisabledLoadDataOnShift;
	}
	else
	{
		shifterConfig.timerPolarity = kFLEXIO_ShifterTimerPolarityOnNegitive;
		shifterConfig.shifterStop   = kFLEXIO_ShifterStopBitDisable;
		shifterConfig.shifterStart  = kFLEXIO_ShifterStartBitDisabledLoadDataOnEnable;
	}
	FLEXIO_SetShifterConfig(base->flexioBase, base->shifterIndex[0], &shifterConfig);

	/* 2. Configure the shifter 1 for rx. */
	shifterConfig.timerSelect  = base->timerIndex[0];
	shifterConfig.pinConfig    = kFLEXIO_PinConfigOutputDisabled;
	shifterConfig.pinSelect    = base->SDIPinIndex;
	shifterConfig.pinPolarity  = kFLEXIO_PinActiveHigh;
	shifterConfig.shifterMode  = kFLEXIO_ShifterModeReceive;
	shifterConfig.inputSource  = kFLEXIO_ShifterInputFromPin;
	shifterConfig.shifterStop  = kFLEXIO_ShifterStopBitDisable;
	shifterConfig.shifterStart = kFLEXIO_ShifterStartBitDisabledLoadDataOnEnable;
	if (pha)
	{
		shifterConfig.timerPolarity = kFLEXIO_ShifterTimerPolarityOnNegitive;
	}
	else
	{
		shifterConfig.timerPolarity = kFLEXIO_ShifterTimerPolarityOnPositive;
	}
	FLEXIO_SetShifterConfig(base->flexioBase, base->shifterIndex[1], &shifterConfig);

	/*3. Configure the timer 0 for SCK. */
	timerConfig.triggerSelect   = FLEXIO_TIMER_TRIGGER_SEL_SHIFTnSTAT(base->shifterIndex[0]);
	timerConfig.triggerPolarity = kFLEXIO_TimerTriggerPolarityActiveLow;
	timerConfig.triggerSource   = kFLEXIO_TimerTriggerSourceInternal;
	timerConfig.pinConfig       = kFLEXIO_PinConfigOutput;
	timerConfig.pinSelect       = base->SCKPinIndex;
	timerConfig.pinPolarity     = pol ? kFLEXIO_PinActiveLow : kFLEXIO_PinActiveHigh;;
	timerConfig.timerMode       = kFLEXIO_TimerModeDual8BitBaudBit;
	timerConfig.timerOutput     = kFLEXIO_TimerOutputZeroNotAffectedByReset;
	timerConfig.timerDecrement  = kFLEXIO_TimerDecSrcOnFlexIOClockShiftTimerOutput;
	timerConfig.timerReset      = kFLEXIO_TimerResetNever;
	timerConfig.timerDisable    = kFLEXIO_TimerDisableOnTimerCompare;
	timerConfig.timerEnable     = kFLEXIO_TimerEnableOnTriggerHigh;
	timerConfig.timerStop       = kFLEXIO_TimerStopBitEnableOnTimerDisable;
	timerConfig.timerStart      = kFLEXIO_TimerStartBitEnabled;

	timerDiv = (uint16_t)(src_clock / baudrate);
	timerDiv = timerDiv / 2U - 1U;

	timerCmp = ((uint16_t)mode * 2U - 1U) << 8U;
	timerCmp |= timerDiv;

	timerConfig.timerCompare = timerCmp;

	FLEXIO_SetTimerConfig(base->flexioBase, base->timerIndex[0], &timerConfig);
}
#endif


static int spi_mcux_configure(const struct device *dev,
			      const struct spi_config *spi_cfg)
{
	const struct spi_mcux_config *config = dev->config;
	struct spi_mcux_data *data = dev->data;
	FLEXIO_SPI_Type *flexio_spi = (FLEXIO_SPI_Type *)&config->flexio_spi;
	flexio_spi_master_config_t master_config;
	uint32_t clock_freq;
	uint32_t word_size;

	if (spi_context_configured(&data->ctx, spi_cfg)) {
		/* This configuration is already in use */
		return 0;
	}

	FLEXIO_SPI_MasterGetDefaultConfig(&master_config);

	word_size = SPI_WORD_SIZE_GET(spi_cfg->operation);
	if ((word_size != 8) && (word_size != 16)) {
		LOG_ERR("Word size %d must be 8 or 16", word_size);
		return -EINVAL;
	}

	if (spi_cfg->operation & SPI_TRANSFER_LSB) {
    if (word_size == 8) {
      data->transfer_flags = kFLEXIO_SPI_8bitLsb;
    } else {
      data->transfer_flags = kFLEXIO_SPI_16bitLsb;
    }
  } else {
    if (word_size == 8) {
      data->transfer_flags = kFLEXIO_SPI_8bitMsb;
    } else {
      data->transfer_flags = kFLEXIO_SPI_16bitMsb;
    }
  }

	if (clock_control_get_rate(config->clock_dev, config->clock_subsys,
				   &clock_freq)) {
		return -EINVAL;
	}

#if (USE_FSL_HAL_FOR_INIT != 0)
  master_config.dataMode = (word_size == 8) 
                           ? kFLEXIO_SPI_8BitMode 
                           : kFLEXIO_SPI_16BitMode;

  /* No CPOL for FlexIO_SPI */

	master_config.phase =
		(SPI_MODE_GET(spi_cfg->operation) & SPI_MODE_CPHA)
		? kFLEXIO_SPI_ClockPhaseSecondEdge
		: kFLEXIO_SPI_ClockPhaseFirstEdge;

	master_config.baudRate_Bps = spi_cfg->frequency;

	FLEXIO_SPI_MasterInit(flexio_spi, &master_config, clock_freq);
#else
	spi_flexio_master_init(flexio_spi, 
												 (SPI_MODE_GET(spi_cfg->operation) & SPI_MODE_CPOL), 
												 (SPI_MODE_GET(spi_cfg->operation) & SPI_MODE_CPHA),
												 (word_size == 8) ? kFLEXIO_SPI_8BitMode : kFLEXIO_SPI_16BitMode,
												 clock_freq, spi_cfg->frequency);
#endif

	FLEXIO_SPI_MasterTransferCreateHandle(flexio_spi, &data->handle,
					 spi_mcux_master_transfer_callback,
					 data);

	/* No SetDummyData() for FlexIO_SPI */

	data->ctx.config = spi_cfg;
	spi_context_cs_configure(&data->ctx);

	return 0;
}


static int transceive(const struct device *dev,
		      const struct spi_config *spi_cfg,
		      const struct spi_buf_set *tx_bufs,
		      const struct spi_buf_set *rx_bufs,
		      bool asynchronous,
		      struct k_poll_signal *signal)
{
	struct spi_mcux_data *data = dev->data;
	int ret;

	spi_context_lock(&data->ctx, asynchronous, signal, spi_cfg);

	ret = spi_mcux_configure(dev, spi_cfg);
	if (ret) {
		goto out;
	}

	spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, 1);

	spi_context_cs_control(&data->ctx, true);

	spi_mcux_transfer_next_packet(dev);

	ret = spi_context_wait_for_completion(&data->ctx);
out:
	spi_context_release(&data->ctx, ret);

	return ret;
}

static int spi_mcux_transceive(const struct device *dev,
			       const struct spi_config *spi_cfg,
			       const struct spi_buf_set *tx_bufs,
			       const struct spi_buf_set *rx_bufs)
{
	return transceive(dev, spi_cfg, tx_bufs, rx_bufs, false, NULL);
}

#ifdef CONFIG_SPI_ASYNC
static int spi_mcux_transceive_async(const struct device *dev,
				     const struct spi_config *spi_cfg,
				     const struct spi_buf_set *tx_bufs,
				     const struct spi_buf_set *rx_bufs,
				     struct k_poll_signal *async)
{
	return transceive(dev, spi_cfg, tx_bufs, rx_bufs, true, async);
}
#endif /* CONFIG_SPI_ASYNC */


static int spi_mcux_release(const struct device *dev,
			    const struct spi_config *spi_cfg)
{
	struct spi_mcux_data *data = dev->data;

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}


static int spi_mcux_init(const struct device *dev)
{
	const struct spi_mcux_config *config = dev->config;
	struct spi_mcux_data *data = dev->data;

	config->irq_config_func(dev);

	spi_context_unlock_unconditionally(&data->ctx);

	data->dev = dev;

	return 0;
}


static const struct spi_driver_api spi_mcux_driver_api = {
	.transceive = spi_mcux_transceive,
#ifdef CONFIG_SPI_ASYNC
	.transceive_async = spi_mcux_transceive_async,
#endif
	.release = spi_mcux_release,
};


#define SPI_MCUX_FLEXIO_SPI_INIT(n)						\
	static void spi_mcux_config_func_##n(const struct device *dev);	\
									\
	static const struct spi_mcux_config spi_mcux_config_##n = {	\
		.flexio_spi.flexioBase = (FLEXIO_Type *) DT_REG_ADDR(DT_INST_PHANDLE(n, flexio)),		\
		.flexio_spi.SDOPinIndex  = DT_INST_PROP(n, sdo_pin),\
		.flexio_spi.SDIPinIndex  = DT_INST_PROP(n, sdi_pin),\
		.flexio_spi.SCKPinIndex  = DT_INST_PROP(n, sck_pin),\
		.flexio_spi.CSnPinIndex  = DT_INST_PROP(n, cs_pin), \
		.flexio_spi.shifterIndex = DT_INST_PROP(n, shifters), \
		.flexio_spi.timerIndex   = DT_INST_PROP(n, timers), \
		.clock_dev = DEVICE_DT_GET(DT_CLOCKS_CTLR(DT_INST_PHANDLE(n, flexio))),	\
		.clock_subsys =						\
		(clock_control_subsys_t)DT_CLOCKS_CELL(DT_INST_PHANDLE(n, flexio), name),	\
		.irq_config_func = spi_mcux_config_func_##n,		\
	};								\
									\
	static struct spi_mcux_data spi_mcux_data_##n = {		\
		SPI_CONTEXT_INIT_LOCK(spi_mcux_data_##n, ctx),		\
		SPI_CONTEXT_INIT_SYNC(spi_mcux_data_##n, ctx),		\
	};								\
									\
	DEVICE_DT_INST_DEFINE(n, &spi_mcux_init, NULL,			\
			    &spi_mcux_data_##n,				\
			    &spi_mcux_config_##n, POST_KERNEL,		\
			    CONFIG_KERNEL_INIT_PRIORITY_DEVICE,		\
			    &spi_mcux_driver_api);			\
									\
	static void spi_mcux_config_func_##n(const struct device *dev)	\
	{								\
		IRQ_CONNECT(DT_IRQN(DT_INST_PHANDLE(n, flexio)), DT_IRQ(DT_INST_PHANDLE(n, flexio), priority),	\
			    spi_mcux_isr, DEVICE_DT_INST_GET(n), 0);	\
									\
		irq_enable(DT_IRQN(DT_INST_PHANDLE(n, flexio)));				\
	}

DT_INST_FOREACH_STATUS_OKAY(SPI_MCUX_FLEXIO_SPI_INIT)

