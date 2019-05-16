// SPDX-License-Identifier: GPL-2.0
/*
 * Novatek NT39016 TFT LCD panel driver
 * Copyright (C) 2017, Maarten ter Huurne <maarten@treewalker.org>
 * Copyright (C) 2019, Paul Cercueil <paul@crapouillou.net>
 */

#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <uapi/linux/media-bus-format.h>


enum nt39016_regs {
	NT39016_REG_SYSTEM,
	NT39016_REG_TIMING,
	NT39016_REG_OP,
	NT39016_REG_DATA_IN,
	NT39016_REG_SRC_TIMING_DELAY,
	NT39016_REG_GATE_TIMING_DELAY,
	NT39016_REG_RESERVED,
	NT39016_REG_INITIAL_FUNC,
	NT39016_REG_CONTRAST,
	NT39016_REG_BRIGHTNESS,
	NT39016_REG_HUE_SATURATION,
	NT39016_REG_RB_SUBCONTRAST,
	NT39016_REG_R_SUBBRIGHTNESS,
	NT39016_REG_B_SUBBRIGHTNESS,
	NT39016_REG_VCOMDC,
	NT39016_REG_VCOMAC,
	NT39016_REG_VGAM2,
	NT39016_REG_VGAM34,
	NT39016_REG_VGAM56,
	NT39016_REG_VCOMDC_TRIM = 0x1e,
	NT39016_REG_DISPLAY_MODE = 0x20,
};

struct nt39016 {
	struct drm_panel drm_panel;
	struct device *dev;
	struct regmap *map;
	struct regulator *supply;

	struct gpio_desc *reset_gpio;

	struct backlight_device *backlight;
};

static const struct drm_display_mode nt39016_display_mode = {
	.clock = 6000,
	.hdisplay = 320,
	.hsync_start = 320 + 10,
	.hsync_end = 320 + 10 + 50,
	.htotal = 320 + 10 + 50 + 20,
	.vdisplay = 240,
	.vsync_start = 240 + 5,
	.vsync_end = 240 + 5 + 1,
	.vtotal = 240 + 5 + 1 + 4,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static inline struct nt39016 *to_nt39016(struct drm_panel *panel)
{
	return container_of(panel, struct nt39016, drm_panel);
}

#define RV(REG, VAL) { .reg = (REG), .def = (VAL), .delay_us = 2 }
static const struct reg_sequence nt39016_panel_regs[] = {
	RV(0x00, 0x04), RV(0x01, 0x00), RV(0x02, 0x03), RV(0x03, 0xCC),
	RV(0x04, 0x46), RV(0x05, 0x05), RV(0x06, 0x00), RV(0x07, 0x00),
	RV(0x08, 0x08), RV(0x09, 0x40), RV(0x0A, 0x88), RV(0x0B, 0x88),
	RV(0x0C, 0x20), RV(0x0D, 0x20), RV(0x0E, 0x67), RV(0x0F, 0xA4),
	RV(0x10, 0x04), RV(0x11, 0x24), RV(0x12, 0x24), RV(0x20, 0x00),
};
#undef RV

static const struct regmap_range nt39016_regmap_no_ranges[] = {
	regmap_reg_range(0x13, 0x1D),
	regmap_reg_range(0x1F, 0x1F),
};

static const struct regmap_access_table nt39016_regmap_access_table = {
	.no_ranges = nt39016_regmap_no_ranges,
	.n_no_ranges = ARRAY_SIZE(nt39016_regmap_no_ranges),
};

static const struct regmap_config nt39016_regmap_config = {
	.reg_bits = 6,
	.pad_bits = 2,
	.val_bits = 8,

	.max_register = NT39016_REG_DISPLAY_MODE,
	.wr_table = &nt39016_regmap_access_table,
	.write_flag_mask = 0x02,

	.cache_type = REGCACHE_FLAT,
};

static int nt39016_prepare(struct drm_panel *drm_panel)
{
	struct nt39016 *panel = to_nt39016(drm_panel);
	int err;

	err = regulator_enable(panel->supply);
	if (err) {
		dev_err(panel->dev, "Failed to enable power supply: %i", err);
		return err;
	}

	/*
	 * Reset the NT39016.
	 * The documentation says the reset pulse should be at least 40 us to
	 * pass the glitch filter, but when testing I see some resets fail and
	 * some succeed when using a 70 us delay, so we use 100 us instead.
	 */
	gpiod_set_value_cansleep(panel->reset_gpio, 1);
	usleep_range(100, 1000);
	gpiod_set_value_cansleep(panel->reset_gpio, 0);
	udelay(2);

	/* Init all registers. */
	err = regmap_multi_reg_write(panel->map, nt39016_panel_regs,
				     ARRAY_SIZE(nt39016_panel_regs));
	if (err) {
		dev_err(panel->dev, "Failed to init registers: %i", err);
		goto err_disable_regulator;
	}

	return 0;

err_disable_regulator:
	regulator_disable(panel->supply);
	return err;
}

static int nt39016_unprepare(struct drm_panel *drm_panel)
{
	struct nt39016 *panel = to_nt39016(drm_panel);

	gpiod_set_value_cansleep(panel->reset_gpio, 1);

	regulator_disable(panel->supply);

	return 0;
}

static int nt39016_enable(struct drm_panel *drm_panel)
{
	struct nt39016 *panel = to_nt39016(drm_panel);
	int err;

	err = regmap_write(panel->map, 0x00, 0x07);
	if (err) {
		dev_err(panel->dev, "Unable to enable panel: %i", err);
		return err;
	}

	/* Wait for the picture to be ready before enabling backlight */
	msleep(150);

	if (panel->backlight) {
		panel->backlight->props.state &= ~BL_CORE_FBBLANK;
		panel->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(panel->backlight);
	}

	return 0;
}

static int nt39016_disable(struct drm_panel *drm_panel)
{
	struct nt39016 *panel = to_nt39016(drm_panel);
	int err;

	if (panel->backlight) {
		panel->backlight->props.power = FB_BLANK_POWERDOWN;
		panel->backlight->props.state |= BL_CORE_FBBLANK;
		backlight_update_status(panel->backlight);
	}

	err = regmap_write(panel->map, 0x00, 0x05);
	if (err) {
		dev_err(panel->dev, "Unable to disable panel: %i", err);
		return err;
	}

	return 0;
}

static int nt39016_get_modes(struct drm_panel *drm_panel)
{
	struct drm_connector *connector = drm_panel->connector;
	struct drm_display_mode *mode;
	u32 bus_format = MEDIA_BUS_FMT_RGB888_1X24;

	mode = drm_mode_duplicate(drm_panel->drm, &nt39016_display_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.bpc = 8;
	connector->display_info.width_mm = 71;
	connector->display_info.height_mm = 53;

	drm_display_info_set_bus_formats(&connector->display_info,
					 &bus_format, 1);
	connector->display_info.bus_flags = DRM_BUS_FLAG_PIXDATA_NEGEDGE;

	return 1;
}

static const struct drm_panel_funcs nt39016_funcs = {
	.prepare	= nt39016_prepare,
	.unprepare	= nt39016_unprepare,
	.enable		= nt39016_enable,
	.disable	= nt39016_disable,
	.get_modes	= nt39016_get_modes,
};

static int nt39016_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct nt39016 *panel;
	int err;

	panel = devm_kzalloc(dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;

	panel->dev = dev;
	spi_set_drvdata(spi, panel);

	panel->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(panel->supply)) {
		dev_err(dev, "Failed to get power supply");
		return PTR_ERR(panel->supply);
	}

	panel->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(panel->reset_gpio)) {
		dev_err(dev, "Failed to get reset GPIO");
		return PTR_ERR(panel->reset_gpio);
	}

	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_3 | SPI_3WIRE;
	err = spi_setup(spi);
	if (err) {
		dev_err(dev, "Failed to setup SPI");
		return err;
	}

	panel->map = devm_regmap_init_spi(spi, &nt39016_regmap_config);
	if (IS_ERR(panel->map)) {
		dev_err(dev, "Failed to init regmap");
		return PTR_ERR(panel->map);
	}

	panel->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(panel->backlight)) {
		err = PTR_ERR(panel->backlight);
		if (err != -EPROBE_DEFER)
			dev_err(dev, "Failed to get backlight handle");
		return err;
	}

	drm_panel_init(&panel->drm_panel);
	panel->drm_panel.dev = dev;
	panel->drm_panel.funcs = &nt39016_funcs;

	err = drm_panel_add(&panel->drm_panel);
	if (err < 0) {
		dev_err(dev, "Failed to register panel");
		goto err_free_backlight;
	}

	return 0;

err_free_backlight:
	if (panel->backlight)
		put_device(&panel->backlight->dev);

	return err;
}

static int nt39016_remove(struct spi_device *spi)
{
	struct nt39016 *panel = spi_get_drvdata(spi);

	drm_panel_remove(&panel->drm_panel);

	nt39016_disable(&panel->drm_panel);
	nt39016_unprepare(&panel->drm_panel);

	if (panel->backlight)
		put_device(&panel->backlight->dev);

	return 0;
}

static const struct of_device_id nt39016_of_match[] = {
	{ .compatible = "novatek,nt39016" },
	{},
};
MODULE_DEVICE_TABLE(of, nt39016_of_match);

static struct spi_driver nt39016_driver = {
	.driver = {
		.name = "nt39016",
		.of_match_table = nt39016_of_match,
	},
	.probe = nt39016_probe,
	.remove = nt39016_remove,
};

module_spi_driver(nt39016_driver);

MODULE_AUTHOR("Maarten ter Huurne <maarten@treewalker.org>");
MODULE_AUTHOR("Paul Cercueil <paul@crapouillou.net>");
MODULE_LICENSE("GPL v2");
