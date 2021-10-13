/*
 * Copyright (c) 2019, NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT fsl_imx6sx_lcdif

#include <drivers/display.h>
#include <fsl_elcdif.h>
#include <fsl_pxp.h>

#ifdef CONFIG_HAS_MCUX_CACHE
#include <fsl_cache.h>
#endif

#include <logging/log.h>
#include <timing/timing.h>

LOG_MODULE_REGISTER(display_mcux_elcdif, CONFIG_DISPLAY_LOG_LEVEL);

#define APP_IMG_WIDTH  CONFIG_MCUX_ELCDIF_PANEL_RGB_WIDTH
#define APP_IMG_HEIGHT CONFIG_MCUX_ELCDIF_PANEL_RGB_HEIGHT

#if (CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT == CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT_RGB565) ||\
    (CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT == CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT_BGR565)

typedef uint16_t pixel_t;
#define APP_BPP            2U /* Use 16-bit RGB565 format. */
#define APP_PXP_PS_FORMAT  kPXP_PsPixelFormatRGB565
#define APP_PXP_AS_FORMAT  kPXP_AsPixelFormatRGB565
#define APP_PXP_OUT_FORMAT kPXP_OutputPixelFormatRGB565
#define APP_DC_FORMAT      kVIDEO_PixelFormatRGB565

#else

typedef uint32_t pixel_t;
#define APP_BPP            4U /* Use 32-bit XRGB888 format. */
#define APP_PXP_PS_FORMAT  kPXP_PsPixelFormatRGB888
#define APP_PXP_AS_FORMAT  kPXP_AsPixelFormatRGB888
#define APP_PXP_OUT_FORMAT kPXP_OutputPixelFormatRGB888
#define APP_DC_FORMAT      kVIDEO_PixelFormatXRGB8888

#endif

#if APP_IMG_WIDTH > APP_IMG_HEIGHT
#define APP_PS_SIZE APP_IMG_HEIGHT
#else
#define APP_PS_SIZE APP_IMG_WIDTH
#endif

#define APP_PS_ULC_X ((APP_IMG_WIDTH / 2) - (APP_PS_SIZE / 2))
#define APP_PS_ULC_Y ((APP_IMG_HEIGHT / 2) - (APP_PS_SIZE / 2))
#define APP_PS_LRC_X ((APP_IMG_WIDTH / 2) + (APP_PS_SIZE / 2) - 1U)
#define APP_PS_LRC_Y ((APP_IMG_HEIGHT / 2) + (APP_PS_SIZE / 2) - 1U)

static struct mcux_elcdif_data mcux_elcdif_data_1;

static uint8_t pxp_buf[CONFIG_MCUX_ELCDIF_PANEL_RGB_WIDTH * CONFIG_MCUX_ELCDIF_PANEL_RGB_HEIGHT * CONFIG_MCUX_ELCDIF_PANEL_BITS_IN_PIXEL / 8u];

K_HEAP_DEFINE(mcux_elcdif_pool,
	      CONFIG_MCUX_ELCDIF_POOL_BLOCK_MAX * CONFIG_MCUX_ELCDIF_POOL_BLOCK_NUM);

struct mcux_elcdif_config {
	LCDIF_Type *base;
	void (*irq_config_func)(const struct device *dev);
	elcdif_rgb_mode_config_t rgb_mode;
	enum display_pixel_format pixel_format;
	uint8_t bits_per_pixel;
	enum display_orientation orientation;
};

struct mcux_mem_block {
	void *data;
};

struct mcux_elcdif_data {
	struct mcux_mem_block fb[2];
	struct k_sem sem;
	size_t pixel_bytes;
	size_t fb_bytes;
	uint8_t write_idx;
};

static int mcux_elcdif_write(const struct device *dev, const uint16_t x,
			     const uint16_t y,
			     const struct display_buffer_descriptor *desc,
			     const void *buf)
{
	const struct mcux_elcdif_config *config = dev->config;
	struct mcux_elcdif_data *data = dev->data;

	uint8_t write_idx = data->write_idx;
	uint8_t read_idx = !write_idx;

	int h_idx, w_idx;
	const uint8_t *src;
	uint8_t *dst;

  timing_t start_time, end_time;
  uint64_t cycles1, cycles2;

  timing_init();
  timing_start();

  start_time = timing_counter_get();

	__ASSERT((data->pixel_bytes * desc->pitch * desc->height) <=
		 desc->buf_size, "Input buffer too small");

	LOG_DBG("W=%d, H=%d, @%d,%d", desc->width, desc->height, x, y);

	k_sem_take(&data->sem, K_FOREVER);

	// memcpy(data->fb[write_idx].data, data->fb[read_idx].data,
	//        data->fb_bytes);

	src = buf;
	dst = pxp_buf;
	dst += data->pixel_bytes * (y * config->rgb_mode.panelHeight + x);

	for (h_idx = 0; h_idx < desc->height; h_idx++) {
		memcpy(dst, src, data->pixel_bytes * desc->width);
		src += data->pixel_bytes * desc->pitch;
		dst += data->pixel_bytes * config->rgb_mode.panelHeight;
	}

  end_time = timing_counter_get();

  cycles1 = timing_cycles_get(&start_time, &end_time);
  start_time = end_time;

	// src = buf;
	// if(config->orientation == DISPLAY_ORIENTATION_NORMAL)
	// {
	// 	dst = data->fb[data->write_idx].data;
	// 	dst += data->pixel_bytes * (y * config->rgb_mode.panelWidth + x);

	// 	for (h_idx = 0; h_idx < desc->height; h_idx++) {
	// 		memcpy(dst, src, data->pixel_bytes * desc->width);
	// 		src += data->pixel_bytes * desc->pitch;
	// 		dst += data->pixel_bytes * config->rgb_mode.panelWidth;
	// 	}
	// }
	// else
	// {
	// 	for (h_idx = 0; h_idx < desc->height; h_idx++) {
	// 		int offs = 0;
	// 		dst = data->fb[data->write_idx].data;
	// 		if(config->orientation == DISPLAY_ORIENTATION_ROTATED_90)
	// 			offs = ((x + 1) * config->rgb_mode.panelWidth - (y + h_idx) - 1);
	// 		else  if(config->orientation == DISPLAY_ORIENTATION_ROTATED_180)
	// 			offs = ((config->rgb_mode.panelHeight - (y + h_idx)) * config->rgb_mode.panelWidth - x - 1);
	// 		else  if(config->orientation == DISPLAY_ORIENTATION_ROTATED_270)
	// 			offs = ((config->rgb_mode.panelHeight - x - 1) * config->rgb_mode.panelWidth + (y + h_idx));
	// 		dst += (data->pixel_bytes * offs);
	// 		for(w_idx = 0; w_idx < desc->pitch; w_idx++) {
	// 			memcpy(dst, src, data->pixel_bytes);
	// 			if(config->orientation == DISPLAY_ORIENTATION_ROTATED_90)
	// 				offs = config->rgb_mode.panelWidth;
	// 			else  if(config->orientation == DISPLAY_ORIENTATION_ROTATED_180)
	// 				offs = -1;
	// 			else  if(config->orientation == DISPLAY_ORIENTATION_ROTATED_270)
	// 				offs = -config->rgb_mode.panelWidth;
	// 			else
	// 				offs = 0;
	// 			dst += (data->pixel_bytes * offs);
	// 			src += data->pixel_bytes;
	// 		}
	// 	}
	// }
  // memcpy(data->fb[write_idx].data, pxp_buf, data->fb_bytes);

  {
	  const pxp_ps_buffer_config_t psBufferConfig = {
		  .pixelFormat =
#if CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT == CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT_RGB888
			  kPXP_PsPixelFormatRGB888,
#elif CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT == CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT_RGB565
			  kPXP_PsPixelFormatRGB565,
#elif CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT == CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT_BGR565
			  kPXP_PsPixelFormatRGB565,
#endif
		  .swapByte = false,
		  .bufferAddr = (uint32_t)pxp_buf,
		  .bufferAddrU = 0U,
		  .bufferAddrV = 0U,
		  .pitchBytes = CONFIG_MCUX_ELCDIF_PANEL_RGB_HEIGHT * APP_BPP,
	  };

	  PXP_SetProcessSurfaceBackGroundColor(PXP, 0xFFFFFFFFU);
    PXP_SetProcessSurfaceBufferConfig(PXP, &psBufferConfig);
	  PXP_SetProcessSurfacePosition(  PXP,  0,
                                          0,
                                          APP_IMG_WIDTH - 1,
                                          APP_IMG_HEIGHT - 1);
  }

  /* Output config. */
  static pxp_output_buffer_config_t outputBufferConfig;

  outputBufferConfig.pixelFormat = APP_PXP_OUT_FORMAT;
  outputBufferConfig.interlacedMode = kPXP_OutputProgressive;
  outputBufferConfig.buffer0Addr = (uint32_t)data->fb[write_idx].data;
  outputBufferConfig.buffer1Addr = 0U;
  outputBufferConfig.pitchBytes = APP_IMG_WIDTH * APP_BPP;
  outputBufferConfig.width = APP_IMG_WIDTH;
  outputBufferConfig.height = APP_IMG_HEIGHT;
  PXP_SetOutputBufferConfig(PXP, &outputBufferConfig);
  PXP_EnableCsc1(PXP, false);

  pxp_rotate_degree_t rot = kPXP_Rotate0;
  if (config->orientation == DISPLAY_ORIENTATION_ROTATED_90)
	  rot = kPXP_Rotate90;
  else if (config->orientation == DISPLAY_ORIENTATION_ROTATED_180)
	  rot = kPXP_Rotate180;
  else if (config->orientation == DISPLAY_ORIENTATION_ROTATED_270)
	  rot = kPXP_Rotate270;

  PXP_SetRotateConfig(PXP, kPXP_RotateProcessSurface, rot, kPXP_FlipDisable);
  PXP_Start(PXP);
  while (!(kPXP_CompleteFlag & PXP_GetStatusFlags(PXP))) {
  }

  PXP_ClearStatusFlags(PXP, kPXP_CompleteFlag);

#ifdef CONFIG_HAS_MCUX_CACHE
	DCACHE_CleanByRange((uint32_t) data->fb[write_idx].data,
			    data->fb_bytes);
#endif

	ELCDIF_SetNextBufferAddr(config->base,
				 (uint32_t) data->fb[write_idx].data);
  end_time = timing_counter_get();
  cycles2 = timing_cycles_get(&start_time, &end_time);
  timing_stop();
	LOG_DBG("t1=%d, t2=%d", timing_cycles_to_ns(cycles1)/1000, timing_cycles_to_ns(cycles2)/1000);

	data->write_idx = read_idx;

	return 0;
}

static int mcux_elcdif_read(const struct device *dev, const uint16_t x,
			    const uint16_t y,
			    const struct display_buffer_descriptor *desc,
			    void *buf)
{
	LOG_ERR("Read not implemented");
	return -ENOTSUP;
}

static void *mcux_elcdif_get_framebuffer(const struct device *dev)
{
	LOG_ERR("Direct framebuffer access not implemented");
	return NULL;
}

static int mcux_elcdif_display_blanking_off(const struct device *dev)
{
	LOG_ERR("Display blanking control not implemented");
	return -ENOTSUP;
}

static int mcux_elcdif_display_blanking_on(const struct device *dev)
{
	LOG_ERR("Display blanking control not implemented");
	return -ENOTSUP;
}

static int mcux_elcdif_set_brightness(const struct device *dev,
				      const uint8_t brightness)
{
	LOG_WRN("Set brightness not implemented");
	return -ENOTSUP;
}

static int mcux_elcdif_set_contrast(const struct device *dev,
				    const uint8_t contrast)
{
	LOG_ERR("Set contrast not implemented");
	return -ENOTSUP;
}

static int mcux_elcdif_set_pixel_format(const struct device *dev,
					const enum display_pixel_format
					pixel_format)
{
	const struct mcux_elcdif_config *config = dev->config;

	if (pixel_format == config->pixel_format) {
		return 0;
	}
	LOG_ERR("Pixel format change not implemented");
	return -ENOTSUP;
}

static int mcux_elcdif_set_orientation(const struct device *dev,
		const enum display_orientation orientation)
{
	if (orientation == DISPLAY_ORIENTATION_NORMAL) {
		return 0;
	}
	LOG_ERR("Changing display orientation not implemented");
	return -ENOTSUP;
}

static void mcux_elcdif_get_capabilities(const struct device *dev,
		struct display_capabilities *capabilities)
{
	const struct mcux_elcdif_config *config = dev->config;

	memset(capabilities, 0, sizeof(struct display_capabilities));
	if( (config->orientation == DISPLAY_ORIENTATION_ROTATED_90) ||\
	    (config->orientation == DISPLAY_ORIENTATION_ROTATED_270)) {
		capabilities->x_resolution = config->rgb_mode.panelHeight;
		capabilities->y_resolution = config->rgb_mode.panelWidth;
	}
	else {
		capabilities->x_resolution = config->rgb_mode.panelWidth;
		capabilities->y_resolution = config->rgb_mode.panelHeight;
	}
	capabilities->supported_pixel_formats = config->pixel_format;
	capabilities->current_pixel_format = config->pixel_format;
	capabilities->current_orientation = config->orientation;
}

static void mcux_elcdif_isr(const struct device *dev)
{
	const struct mcux_elcdif_config *config = dev->config;
	struct mcux_elcdif_data *data = dev->data;
	uint32_t status;

	status = ELCDIF_GetInterruptStatus(config->base);
	ELCDIF_ClearInterruptStatus(config->base, status);

	k_sem_give(&data->sem);
}

static int mcux_elcdif_init(const struct device *dev)
{
	const struct mcux_elcdif_config *config = dev->config;
	struct mcux_elcdif_data *data = dev->data;
	int i;

	elcdif_rgb_mode_config_t rgb_mode = config->rgb_mode;

	data->pixel_bytes = config->bits_per_pixel / 8U;
	data->fb_bytes = data->pixel_bytes *
			 rgb_mode.panelWidth * rgb_mode.panelHeight;
	data->write_idx = 1U;

	for (i = 0; i < ARRAY_SIZE(data->fb); i++) {
		data->fb[i].data = k_heap_alloc(&mcux_elcdif_pool,
						data->fb_bytes, K_NO_WAIT);
		if (data->fb[i].data == NULL) {
			LOG_ERR("Could not allocate frame buffer %d", i);
			return -ENOMEM;
		}
		memset(data->fb[i].data, 0, data->fb_bytes);
	}
	rgb_mode.bufferAddr = (uint32_t) data->fb[0].data;
  memset(pxp_buf, 0, sizeof(pxp_buf));

	k_sem_init(&data->sem, 1, 1);

	config->irq_config_func(dev);

	ELCDIF_RgbModeInit(config->base, &rgb_mode);
	ELCDIF_EnableInterrupts(config->base,
				kELCDIF_CurFrameDoneInterruptEnable);
  // default eLCDIF pixel pattern is RGB
  if(data->pixel_bytes == 2)
  {
    // 16 bit output
    if(config->pixel_format == PIXEL_FORMAT_RGB_565)
      ELCDIF_SetPixelPattern(config->base, kELCDIF_PixelPatternRGB);
    else if(config->pixel_format == PIXEL_FORMAT_BGR_565)
      ELCDIF_SetPixelPattern(config->base, kELCDIF_PixelPatternBGR);
  }

  if (config->orientation != DISPLAY_ORIENTATION_NORMAL)
	  PXP_Init(PXP);
  /* Disable AS. */
  PXP_SetAlphaSurfacePosition(PXP, 0xFFFFU, 0xFFFFU, 0U, 0U);

  ELCDIF_RgbModeStart(config->base);

  return 0;
}

static const struct display_driver_api mcux_elcdif_api = {
	.blanking_on = mcux_elcdif_display_blanking_on,
	.blanking_off = mcux_elcdif_display_blanking_off,
	.write = mcux_elcdif_write,
	.read = mcux_elcdif_read,
	.get_framebuffer = mcux_elcdif_get_framebuffer,
	.set_brightness = mcux_elcdif_set_brightness,
	.set_contrast = mcux_elcdif_set_contrast,
	.get_capabilities = mcux_elcdif_get_capabilities,
	.set_pixel_format = mcux_elcdif_set_pixel_format,
	.set_orientation = mcux_elcdif_set_orientation,
};

static void mcux_elcdif_config_func_1(const struct device *dev);

static struct mcux_elcdif_config mcux_elcdif_config_1 = {
	.base = (LCDIF_Type *) DT_INST_REG_ADDR(0),
	.irq_config_func = mcux_elcdif_config_func_1,
	.rgb_mode = {
		.panelWidth = CONFIG_MCUX_ELCDIF_PANEL_RGB_WIDTH,
		.panelHeight = CONFIG_MCUX_ELCDIF_PANEL_RGB_HEIGHT,
		.hsw = CONFIG_MCUX_ELCDIF_PANEL_RGB_HSW,
		.hfp = CONFIG_MCUX_ELCDIF_PANEL_RGB_HFP,
		.hbp = CONFIG_MCUX_ELCDIF_PANEL_RGB_HBP,
		.vsw = CONFIG_MCUX_ELCDIF_PANEL_RGB_VSW,
		.vfp = CONFIG_MCUX_ELCDIF_PANEL_RGB_VFP,
		.vbp = CONFIG_MCUX_ELCDIF_PANEL_RGB_VBP,
		.polarityFlags = 
		#ifdef  CONFIG_MCUX_ELCDIF_PANEL_RGB_POLARITY_FLAG_DE_HIGH
				kELCDIF_DataEnableActiveHigh |
		#else
				kELCDIF_DataEnableActiveLow |
		#endif
		#ifdef CONFIG_MCUX_ELCDIF_PANEL_RGB_POLARITY_FLAG_VSYNC_HIGH
				kELCDIF_VsyncActiveHigh |
		#else
				kELCDIF_VsyncActiveLow |
		#endif
		#ifdef CONFIG_MCUX_ELCDIF_PANEL_RGB_POLARITY_FLAG_HSYNC_HIGH
				kELCDIF_HsyncActiveHigh |
		#else 
				kELCDIF_HsyncActiveLow |
		#endif
		#ifdef CONFIG_MCUX_ELCDIF_PANEL_RGB_POLARITY_FLAG_CLKEDGE_RISING
				kELCDIF_DriveDataOnRisingClkEdge,
		#else
				kELCDIF_DriveDataOnFallingClkEdge,
		#endif
		#if CONFIG_MCUX_ELCDIF_PANEL_RGB_PIXEL_FMT == CONFIG_MCUX_ELCDIF_PANEL_RGB_PIXEL_FMT_RAW8
		.pixelFormat = kELCDIF_PixelFormatRAW8,
		#elif CONFIG_MCUX_ELCDIF_PANEL_RGB_PIXEL_FMT  ==  CONFIG_MCUX_ELCDIF_PANEL_RGB_PIXEL_FMT_RGB565
		.pixelFormat = kELCDIF_PixelFormatRGB565,
		#elif CONFIG_MCUX_ELCDIF_PANEL_RGB_PIXEL_FMT  ==  CONFIG_MCUX_ELCDIF_PANEL_RGB_PIXEL_FMT_RGB666
		.pixelFormat = kELCDIF_PixelFormatRGB666,
		#elif CONFIG_MCUX_ELCDIF_PANEL_RGB_PIXEL_FMT  ==  CONFIG_MCUX_ELCDIF_PANEL_RGB_PIXEL_FMT_RGB8888
		.pixelFormat = kELCDIF_PixelFormatXRGB8888,
		#elif CONFIG_MCUX_ELCDIF_PANEL_RGB_PIXEL_FMT  ==  CONFIG_MCUX_ELCDIF_PANEL_RGB_PIXEL_FMT_RGB888
		.pixelFormat = kELCDIF_PixelFormatRGB888,
		#endif
		#if     CONFIG_MCUX_ELCDIF_PANEL_RGB_DBUS ==  CONFIG_MCUX_ELCDIF_PANEL_RGB_DBUS_8
		.dataBus = kELCDIF_DataBus8Bit,
		#elif   CONFIG_MCUX_ELCDIF_PANEL_RGB_DBUS ==  CONFIG_MCUX_ELCDIF_PANEL_RGB_DBUS_16
		.dataBus = kELCDIF_DataBus16Bit,
		#elif   CONFIG_MCUX_ELCDIF_PANEL_RGB_DBUS ==  CONFIG_MCUX_ELCDIF_PANEL_RGB_DBUS_18
		.dataBus = kELCDIF_DataBus18Bit,
		#elif   CONFIG_MCUX_ELCDIF_PANEL_RGB_DBUS ==  CONFIG_MCUX_ELCDIF_PANEL_RGB_DBUS_24
		.dataBus = kELCDIF_DataBus24Bit,
		#endif
	},
	#if   CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT ==  CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT_RGB888
	.pixel_format = PIXEL_FORMAT_RGB_888,
	#elif CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT ==  CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT_MONO01
	.pixel_format = PIXEL_FORMAT_MONO01,
	#elif CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT ==  CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT_MONO10
	.pixel_format = PIXEL_FORMAT_MONO10,
	#elif CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT ==  CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT_ARGB8888
	.pixel_format = PIXEL_FORMAT_ARGB_8888,
	#elif CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT ==  CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT_RGB565
	.pixel_format = PIXEL_FORMAT_RGB_565,
	#elif CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT ==  CONFIG_MCUX_ELCDIF_PANEL_PIXEL_FMT_BGR565
	.pixel_format = PIXEL_FORMAT_BGR_565,
	#endif
	#ifdef CONFIG_MCUX_ELCDIF_PANEL_BITS_IN_PIXEL
	.bits_per_pixel = CONFIG_MCUX_ELCDIF_PANEL_BITS_IN_PIXEL,
	#endif
	#ifdef CONFIG_MCUX_ELCDIF_PANEL_ORIENTATION_NORMAL
	.orientation = DISPLAY_ORIENTATION_NORMAL,
	#elif defined CONFIG_MCUX_ELCDIF_PANEL_ORIENTATION_ROTATED_90
	.orientation = DISPLAY_ORIENTATION_ROTATED_90,
	#elif defined CONFIG_MCUX_ELCDIF_PANEL_ORIENTATION_ROTATED_180
	.orientation = DISPLAY_ORIENTATION_ROTATED_180,
	#elif defined CONFIG_MCUX_ELCDIF_PANEL_ORIENTATION_ROTATED_270
	.orientation = DISPLAY_ORIENTATION_ROTATED_270,
	#else
	#warning "Select correct display orientation !"
	#endif
};

DEVICE_DT_INST_DEFINE(0,
		    &mcux_elcdif_init,
		    NULL,
		    &mcux_elcdif_data_1, &mcux_elcdif_config_1,
		    POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &mcux_elcdif_api);

static void mcux_elcdif_config_func_1(const struct device *dev)
{
	IRQ_CONNECT(DT_INST_IRQN(0),
		    DT_INST_IRQ(0, priority),
		    mcux_elcdif_isr, DEVICE_DT_INST_GET(0), 0);

	irq_enable(DT_INST_IRQN(0));
}
