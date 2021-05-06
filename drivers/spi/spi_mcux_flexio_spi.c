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

  master_config.dataMode = (word_size == 8) 
                           ? kFLEXIO_SPI_8BitMode 
                           : kFLEXIO_SPI_16BitMode;

  /* No CPOL for FlexIO_SPI */

	master_config.phase =
		(SPI_MODE_GET(spi_cfg->operation) & SPI_MODE_CPHA)
		? kFLEXIO_SPI_ClockPhaseSecondEdge
		: kFLEXIO_SPI_ClockPhaseFirstEdge;

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

	master_config.baudRate_Bps = spi_cfg->frequency;

	if (clock_control_get_rate(config->clock_dev, config->clock_subsys,
				   &clock_freq)) {
		return -EINVAL;
	}

	FLEXIO_SPI_MasterInit(flexio_spi, &master_config, clock_freq);

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

