// SPDX-License-Identifier: GPL-2.0
/*
 * radio-rda5807.c - Driver for using the RDA5807 FM tuner chip via I2C
 *
 * Copyright (c) 2011 Maarten ter Huurne <maarten@treewalker.org>
 *
 * Many thanks to Jérôme VERES for his command line radio application that
 * demonstrates how the chip can be controlled via I2C.
 *
 * The RDA5807 has three ways of accessing registers:
 * - I2C address 0x10: sequential access, RDA5800 style
 * - I2C address 0x11: random access
 * - I2C address 0x60: sequential access, TEA5767 compatible
 */


#include <asm/byteorder.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/of_platform.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>


enum rda5807_reg {
	RDA5807_REG_CHIPID		= 0x00,
	RDA5807_REG_CTRL		= 0x02,
	RDA5807_REG_CHAN		= 0x03,
	RDA5807_REG_IOCFG		= 0x04,
	RDA5807_REG_INTM_THRESH_VOL	= 0x05,
	RDA5807_REG_SEEK_RESULT		= 0x0A,
	RDA5807_REG_SIGNAL		= 0x0B,
};

#define RDA5807_MASK_CTRL_DHIZ		BIT(15)
#define RDA5807_MASK_CTRL_DMUTE		BIT(14)
#define RDA5807_MASK_CTRL_MONO		BIT(13)
#define RDA5807_MASK_CTRL_BASS		BIT(12)
#define RDA5807_MASK_CTRL_SEEKUP	BIT(9)
#define RDA5807_MASK_CTRL_SEEK		BIT(8)
#define RDA5807_MASK_CTRL_SKMODE	BIT(7)
#define RDA5807_MASK_CTRL_CLKMODE	(7 << 4)
#define RDA5807_MASK_CTRL_SOFTRESET	BIT(1)
#define RDA5807_MASK_CTRL_ENABLE	BIT(0)

#define RDA5807_SHIFT_CHAN_WRCHAN	6
#define RDA5807_MASK_CHAN_WRCHAN	(0x3FF << RDA5807_SHIFT_CHAN_WRCHAN)
#define RDA5807_MASK_CHAN_TUNE		BIT(4)
#define RDA5807_SHIFT_CHAN_BAND		2
#define RDA5807_MASK_CHAN_BAND		(0x3 << RDA5807_SHIFT_CHAN_BAND)
#define RDA5807_SHIFT_CHAN_SPACE	0
#define RDA5807_MASK_CHAN_SPACE		(0x3 << RDA5807_SHIFT_CHAN_SPACE)

#define RDA5807_MASK_SEEKRES_COMPLETE	BIT(14)
#define RDA5807_MASK_SEEKRES_FAIL	BIT(13)
#define RDA5807_MASK_SEEKRES_STEREO	BIT(10)
#define RDA5807_MASK_SEEKRES_READCHAN	0x3FF

#define RDA5807_MASK_DEEMPHASIS		BIT(11)

#define RDA5807_SHIFT_VOLUME_DAC	0
#define RDA5807_MASK_VOLUME_DAC		(0xF << RDA5807_SHIFT_VOLUME_DAC)

#define RDA5807_SHIFT_RSSI		9
#define RDA5807_MASK_RSSI		(0x7F << RDA5807_SHIFT_RSSI)

/* Working current: 1.8, 2.1, 2.5 or 3.0 mA. */
#define RDA5807_INPUT_LNA_WC_18		(0 << 0)
#define RDA5807_INPUT_LNA_WC_21		(1 << 0)
#define RDA5807_INPUT_LNA_WC_25		(2 << 0)
#define RDA5807_INPUT_LNA_WC_30		(3 << 0)
/* Use antenna signal connected to LNAN and/or LNAP pin? */
#define RDA5807_LNA_PORT_N		BIT(2)
#define RDA5807_LNA_PORT_P		BIT(3)

/* Ouput analog audio on LOUT+ROUT pins */
#define RDA5807_OUTPUT_AUDIO_ANALOG	BIT(0)
/* Output digital audio using I2S on GPIO1-3 pins */
#define RDA5807_OUTPUT_AUDIO_I2S	BIT(1)
/* Output stereo indicator signal on GPIO3 pin */
#define RDA5807_OUTPUT_STEREO_INDICATOR	BIT(2)

#define RDA5807_FREQ_MIN_KHZ  76000
#define RDA5807_FREQ_MAX_KHZ 108000

struct rda5807_driver {
	struct v4l2_ctrl_handler ctrl_handler;
	struct video_device video_dev;
	struct v4l2_device v4l2_dev;

	struct device *dev;
	struct regmap *map;
	struct regulator *supply;

	u8 input_flags;
	u8 output_flags;
};

static const struct v4l2_file_operations rda5807_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= video_ioctl2,
};

static int rda5807_enable(struct rda5807_driver *radio)
{
	int err;

	err = regmap_update_bits(radio->map, RDA5807_REG_CTRL,
				 RDA5807_MASK_CTRL_ENABLE,
				 RDA5807_MASK_CTRL_ENABLE);
	if (err < 0)
		return err;

	err = regmap_update_bits(radio->map, RDA5807_REG_CHAN,
				 RDA5807_MASK_CHAN_TUNE,
				 RDA5807_MASK_CHAN_TUNE);
	if (err)
		goto err_update_bits;

	/* following the rda5807 programming guide, we
	 * need to wait for 0.5 seconds before tune */
	msleep(500);

	return 0;

err_update_bits:
	regmap_update_bits(radio->map, RDA5807_REG_CTRL,
			   RDA5807_MASK_CTRL_ENABLE, 0);
	return err;
}

static int rda5807_disable(struct rda5807_driver *radio)
{
	return regmap_update_bits(radio->map, RDA5807_REG_CTRL,
				  RDA5807_MASK_CTRL_ENABLE, 0);
}

static inline int rda5807_set_enable(struct rda5807_driver *radio, bool enabled)
{
	if (enabled)
		return rda5807_enable(radio);
	else
		return rda5807_disable(radio);
}

static int rda5807_set_mute(struct rda5807_driver *radio, int muted)
{
	u16 val = muted ? 0 : RDA5807_MASK_CTRL_DMUTE /* disable mute */;

	dev_dbg(radio->dev, "set mute to %d", muted);

	return regmap_update_bits(radio->map, RDA5807_REG_CTRL,
				  RDA5807_MASK_CTRL_DMUTE, val);
}

static int rda5807_set_volume(struct rda5807_driver *radio, int volume)
{
	dev_dbg(radio->dev, "set volume to %d", volume);

	return regmap_update_bits(radio->map, RDA5807_REG_INTM_THRESH_VOL,
				  RDA5807_MASK_VOLUME_DAC,
				  volume << RDA5807_SHIFT_VOLUME_DAC);
}

static int rda5807_set_preemphasis(struct rda5807_driver *radio,
				   enum v4l2_preemphasis preemp)
{
	dev_dbg(radio->dev, "set preemphasis to %d", preemp);

	return regmap_update_bits(radio->map, RDA5807_REG_IOCFG,
				  RDA5807_MASK_DEEMPHASIS,
				  preemp == V4L2_PREEMPHASIS_50_uS
				          ? RDA5807_MASK_DEEMPHASIS : 0);
}

static int rda5807_get_frequency(struct rda5807_driver *radio)
{
	unsigned int val;
	u32 freq_khz;
	int err;

	err = regmap_read(radio->map, RDA5807_REG_SEEK_RESULT, &val);
	if (err < 0)
		return err;

	freq_khz = 50 * (val & RDA5807_MASK_SEEKRES_READCHAN)
		   + RDA5807_FREQ_MIN_KHZ;

	dev_dbg(radio->dev, "get freq of %u kHz", freq_khz);

	return freq_khz;
}

static int rda5807_set_frequency(struct rda5807_driver *radio, u32 freq_khz)
{
	u16 mask = 0;
	u16 val = 0;

	dev_dbg(radio->dev, "set freq to %u kHz", freq_khz);

	if (freq_khz < RDA5807_FREQ_MIN_KHZ)
		return -ERANGE;
	if (freq_khz > RDA5807_FREQ_MAX_KHZ)
		return -ERANGE;

	/* select widest band */
	mask |= RDA5807_MASK_CHAN_BAND;
	val  |= 2 << RDA5807_SHIFT_CHAN_BAND;

	/* select 50 kHz channel spacing */
	mask |= RDA5807_MASK_CHAN_SPACE;
	val  |= 2 << RDA5807_SHIFT_CHAN_SPACE;

	/* select frequency */
	mask |= RDA5807_MASK_CHAN_WRCHAN;
	val  |= ((freq_khz - RDA5807_FREQ_MIN_KHZ + 25) / 50)
			<< RDA5807_SHIFT_CHAN_WRCHAN;

	/* start tune operation */
	mask |= RDA5807_MASK_CHAN_TUNE;
	val  |= RDA5807_MASK_CHAN_TUNE;

	return regmap_update_bits(radio->map, RDA5807_REG_CHAN, mask, val);
}

static int rda5807_seek_frequency(struct rda5807_driver *radio,
				  int upward, int wrap)
{
	unsigned int mask = 0, val = 0, count = 0;
	int ret;

	/*
	 * TODO: Seek threshold is configurable. How should the driver handle
	 *       this configuration?
	 */

	/* seek up or down? */
	mask |= RDA5807_MASK_CTRL_SEEKUP;
	if (upward)
		val |= RDA5807_MASK_CTRL_SEEKUP;

	/* wrap around at band limit? */
	mask |= RDA5807_MASK_CTRL_SKMODE;
	if (!wrap)
		val |= RDA5807_MASK_CTRL_SKMODE;

	/* seek command */
	mask |= RDA5807_MASK_CTRL_SEEK;
	val  |= RDA5807_MASK_CTRL_SEEK;

	ret = regmap_update_bits(radio->map, RDA5807_REG_CTRL, mask, val);
	if (ret < 0)
		return ret;

	for (;;) {
		/*
		 * The programming guide says we should wait for 35 ms for each
		 * frequency tested.
		 */
		msleep(35);

		ret = regmap_read(radio->map, RDA5807_REG_SEEK_RESULT, &val);
		if (ret < 0)
			return ret;

		/* Seek done? */
		if (val & RDA5807_MASK_SEEKRES_COMPLETE)
			return 0;

		/*
		 * Channel spacing is 100 kHz.
		 * TODO: Should we support configurable spacing?
		 */
		count++;
		if (count > (RDA5807_FREQ_MAX_KHZ - RDA5807_FREQ_MIN_KHZ) / 100)
			return -ETIMEDOUT;
	}
}

static inline struct rda5807_driver *ctrl_to_radio(struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct rda5807_driver, ctrl_handler);
}

static int rda5807_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct rda5807_driver *radio = ctrl_to_radio(ctrl);

	switch (ctrl->id) {
	case V4L2_CID_AUDIO_MUTE: {
		/*
		 * Disable the radio while muted, to save power.
		 * TODO: We can't seek while the radio is disabled;
		 *       is that a problem?
		 */
		int err1 = rda5807_set_enable(radio, !ctrl->val);
		int err2 = rda5807_set_mute(radio, ctrl->val);
		return err1 ? err1 : err2;
	}
	case V4L2_CID_AUDIO_VOLUME:
		return rda5807_set_volume(radio, ctrl->val);
	case V4L2_CID_TUNE_PREEMPHASIS:
		return rda5807_set_preemphasis(radio, ctrl->val);
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops rda5807_ctrl_ops = {
	.s_ctrl = rda5807_s_ctrl,
};

static int rda5807_vidioc_querycap(struct file *file, void *fh,
				   struct v4l2_capability *cap)
{
	*cap = (struct v4l2_capability) {
		.driver		= "rda5807",
		.card		= "RDA5807 FM receiver",
		.bus_info	= "I2C",
		.device_caps	= V4L2_CAP_RADIO | V4L2_CAP_TUNER
						 | V4L2_CAP_HW_FREQ_SEEK,
	};
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int rda5807_vidioc_g_audio(struct file *file, void *fh,
				  struct v4l2_audio *a)
{
	if (a->index != 0)
		return -EINVAL;

	*a = (struct v4l2_audio) {
		.name = "Radio",
		.capability = V4L2_AUDCAP_STEREO,
		.mode = 0,
	};

	return 0;
}

static int rda5807_vidioc_g_tuner(struct file *file, void *fh,
				  struct v4l2_tuner *a)
{
	struct rda5807_driver *radio = video_drvdata(file);
	unsigned int seekres, signal;
	u32 rxsubchans;
	int err;

	if (a->index != 0)
		return -EINVAL;

	err = regmap_read(radio->map, RDA5807_REG_SEEK_RESULT, &seekres);
	if (err < 0)
		return err;

	if ((seekres & (RDA5807_MASK_SEEKRES_COMPLETE
						| RDA5807_MASK_SEEKRES_FAIL))
				== RDA5807_MASK_SEEKRES_COMPLETE)
		/* mono/stereo known */
		rxsubchans = seekres & RDA5807_MASK_SEEKRES_STEREO
				? V4L2_TUNER_SUB_STEREO : V4L2_TUNER_SUB_MONO;
	else
		/* mono/stereo unknown */
		rxsubchans = V4L2_TUNER_SUB_MONO | V4L2_TUNER_SUB_STEREO;

	err = regmap_read(radio->map, RDA5807_REG_SIGNAL, &signal);
	if (err < 0)
		return err;

	signal = (signal & RDA5807_MASK_RSSI) >> RDA5807_SHIFT_RSSI;

	*a = (struct v4l2_tuner) {
		.name = "FM",
		.type = V4L2_TUNER_RADIO,
		.capability = V4L2_TUNER_CAP_LOW | V4L2_TUNER_CAP_STEREO,
		/* unit is 1/16 kHz */
		.rangelow   = RDA5807_FREQ_MIN_KHZ * 16,
		.rangehigh  = RDA5807_FREQ_MAX_KHZ * 16,
		.rxsubchans = rxsubchans,
		/* TODO: Implement forced mono (RDA5807_MASK_CTRL_MONO). */
		.audmode = V4L2_TUNER_MODE_STEREO,
		.signal = signal << (16 - 7),
		.afc = 0, /* automatic frequency control */
	};

	return 0;
}

static int rda5807_vidioc_g_frequency(struct file *file, void *fh,
				      struct v4l2_frequency *a)
{
	struct rda5807_driver *radio = video_drvdata(file);
	int freq_khz;

	if (a->tuner != 0)
		return -EINVAL;

	/* This ioctl ignores the type field. */

	freq_khz = rda5807_get_frequency(radio);
	if (freq_khz < 0)
		return freq_khz;

	a->frequency = (__u32)freq_khz * 16;
	return 0;
}

static int rda5807_vidioc_s_frequency(struct file *file, void *fh,
				      const struct v4l2_frequency *a)
{
	struct rda5807_driver *radio = video_drvdata(file);

	if (a->tuner != 0)
		return -EINVAL;
	if (a->type != V4L2_TUNER_RADIO)
		return -EINVAL;

	return rda5807_set_frequency(radio, (a->frequency * 625) / 10000);
}

static int rda5807_vidioc_s_hw_freq_seek(struct file *file, void *fh,
					 const struct v4l2_hw_freq_seek *a)
{
	struct rda5807_driver *radio = video_drvdata(file);

	if (a->tuner != 0)
		return -EINVAL;
	if (a->type != V4L2_TUNER_RADIO)
		return -EINVAL;

	return rda5807_seek_frequency(radio, a->seek_upward, a->wrap_around);
}

static const struct v4l2_ioctl_ops rda5807_ioctl_ops = {
	.vidioc_querycap	= rda5807_vidioc_querycap,
	.vidioc_g_audio		= rda5807_vidioc_g_audio,
	.vidioc_g_tuner		= rda5807_vidioc_g_tuner,
	.vidioc_g_frequency	= rda5807_vidioc_g_frequency,
	.vidioc_s_frequency	= rda5807_vidioc_s_frequency,
	.vidioc_s_hw_freq_seek  = rda5807_vidioc_s_hw_freq_seek,
};

static const char * const rda5807_name = "RDA5807 FM receiver";

static const u16 rda5807_lna_current[] = { 1800, 2100, 2500, 3000 };

static const struct regmap_config rda5807_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = RDA5807_REG_SIGNAL,
};

static int __init_or_module rda5807_i2c_probe(struct i2c_client *client,
					      const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct rda5807_driver *radio;
	u16 lna_current = 2500;
	unsigned int val;
	size_t i;
	int err;

	radio = devm_kzalloc(dev, sizeof(*radio), GFP_KERNEL);
	if (!radio) {
		dev_err(dev, "Failed to allocate driver data");
		return -ENOMEM;
	}

	radio->dev = dev;

	radio->map = devm_regmap_init_i2c(client, &rda5807_regmap_config);
	if (IS_ERR(radio->map)) {
		dev_err(dev, "Failed to create regmap");
		return PTR_ERR(radio->map);
	}

	radio->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(radio->supply)) {
		dev_err(dev, "Failed to get power supply");
		return PTR_ERR(radio->supply);
	}

	device_property_read_u16(dev, "lna-current", &lna_current);
	for (i = 0; i < ARRAY_SIZE(rda5807_lna_current); i++)
		if (rda5807_lna_current[i] == lna_current)
			radio->input_flags = i;

	if (device_property_read_bool(dev, "lnan"))
		radio->input_flags |= RDA5807_LNA_PORT_N;
	if (device_property_read_bool(dev, "lnap"))
		radio->input_flags |= RDA5807_LNA_PORT_P;

	if (device_property_read_bool(dev, "i2s-out"))
		radio->output_flags |= RDA5807_OUTPUT_AUDIO_I2S;
	if (device_property_read_bool(dev, "analog-out"))
		radio->output_flags |= RDA5807_OUTPUT_AUDIO_ANALOG;

	if (!(radio->input_flags & (RDA5807_LNA_PORT_N | RDA5807_LNA_PORT_P)))
		dev_warn(dev, "Both LNA inputs disabled");

	err = regulator_enable(radio->supply);
	if (err) {
		dev_err(dev, "Failed to enable regulator");
		return err;
	}

	err = regmap_read(radio->map, RDA5807_REG_CHIPID, &val);
	if (err < 0) {
		dev_err(dev, "Failed to read chip ID");
		goto err_regulator_disable;
	}

	if ((val & 0xFF00) != 0x5800) {
		dev_err(dev, "Chip ID mismatch: expected 58xx, got %04X", val);
		err = -ENODEV;
		goto err_regulator_disable;
	}

	dev_info(dev, "Found FM radio receiver");

	/* TODO: Resetting the chip would be good. */

	/* Initialize controls. */
	v4l2_ctrl_handler_init(&radio->ctrl_handler, 3);
	v4l2_ctrl_new_std(&radio->ctrl_handler, &rda5807_ctrl_ops,
			  V4L2_CID_AUDIO_MUTE, 0, 1, 1, 1);
	v4l2_ctrl_new_std(&radio->ctrl_handler, &rda5807_ctrl_ops,
			  V4L2_CID_AUDIO_VOLUME, 0, 15, 1, 8);

	/*
	 * TODO: V4L2_CID_TUNE_PREEMPHASIS is based on V4L2_CID_FM_TX_CLASS_BASE
	 *       which suggests it is a transmit control rather than a receive
	 *       control. The register bit we change is called "de-emphasis",
	 *       but there is no de-emphasis control in V4L2.
	 */
	v4l2_ctrl_new_std_menu(&radio->ctrl_handler, &rda5807_ctrl_ops,
			       V4L2_CID_TUNE_PREEMPHASIS,
			       V4L2_PREEMPHASIS_75_uS,
			       BIT(V4L2_PREEMPHASIS_DISABLED),
			       V4L2_PREEMPHASIS_50_uS);
	err = radio->ctrl_handler.error;
	if (err) {
		dev_err(dev, "Failed to init controls handler");
		goto err_ctrl_free;
	}

	strlcpy(radio->v4l2_dev.name, rda5807_name,
		sizeof(radio->v4l2_dev.name));

	err = v4l2_device_register(NULL, &radio->v4l2_dev);
	if (err < 0) {
		dev_err(dev, "Failed to register v4l2 device");
		goto err_ctrl_free;
	}

	radio->video_dev = (struct video_device) {
		.name = "RDA5807 FM receiver",
		.v4l2_dev = &radio->v4l2_dev,
		.ctrl_handler = &radio->ctrl_handler,
		.fops = &rda5807_fops,
		.ioctl_ops = &rda5807_ioctl_ops,
		.release = video_device_release_empty,
	};

	i2c_set_clientdata(client, radio);
	video_set_drvdata(&radio->video_dev, radio);

	err = video_register_device(&radio->video_dev, VFL_TYPE_RADIO, -1);
	if (err < 0) {
		dev_err(dev, "Failed to register video device");
		goto err_ctrl_free;
	}

	/* Configure chip inputs. */
	err = regmap_update_bits(radio->map, RDA5807_REG_INTM_THRESH_VOL,
				 0xF << 4, (radio->input_flags & 0xF) << 4);
	if (err < 0)
		dev_warn(dev, "Failed to configure inputs (%d)", err);

	/* Configure chip outputs. */
	val = 0;
	if (radio->output_flags & RDA5807_OUTPUT_AUDIO_I2S)
		val |= BIT(6);

	err = regmap_update_bits(radio->map, RDA5807_REG_IOCFG, 0x003F, val);
	if (err < 0)
		dev_warn(dev, "Failed to configure outputs (%d)", err);

	val = 0;
	if (radio->output_flags & RDA5807_OUTPUT_AUDIO_ANALOG)
		val |= BIT(15);

	err = regmap_update_bits(radio->map, RDA5807_REG_CTRL, BIT(15), val);
	if (err < 0)
		dev_warn(dev, "Failed to configure outputs (%d)", err);

	err = v4l2_ctrl_handler_setup(&radio->ctrl_handler);
	if (err < 0) {
		dev_err(dev, "Failed to set default control values");
		goto err_video_unreg;
	}

	return 0;

err_video_unreg:
	video_unregister_device(&radio->video_dev);

err_ctrl_free:
	v4l2_ctrl_handler_free(&radio->ctrl_handler);
	video_device_release_empty(&radio->video_dev);

err_regulator_disable:
	regulator_disable(radio->supply);

	return err;
}

static int __exit rda5807_i2c_remove(struct i2c_client *client)
{
	struct rda5807_driver *radio = i2c_get_clientdata(client);

	video_unregister_device(&radio->video_dev);
	v4l2_ctrl_handler_free(&radio->ctrl_handler);
	video_device_release_empty(&radio->video_dev);

	regulator_disable(radio->supply);

	return 0;
}

static int __maybe_unused rda5807_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rda5807_driver *radio = i2c_get_clientdata(client);

	return rda5807_set_enable(radio, 0);
}

static int __maybe_unused rda5807_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rda5807_driver *radio = i2c_get_clientdata(client);
	struct v4l2_ctrl *mute_ctrl = v4l2_ctrl_find(&radio->ctrl_handler,
						     V4L2_CID_AUDIO_MUTE);
	s32 mute_val = v4l2_ctrl_g_ctrl(mute_ctrl);
	bool enabled = !mute_val;

	if (enabled)
		return rda5807_set_enable(radio, enabled);
	else
		return 0;
}


static const struct dev_pm_ops __maybe_unused rda5807_pm_ops = {
	.suspend = rda5807_suspend,
	.resume = rda5807_resume,
};

#ifdef CONFIG_OF
static const struct of_device_id rda5807_dt_ids[] = {
	{ .compatible = "rdamicro,rda5807" },
	{ }
};
MODULE_DEVICE_TABLE(of, rda5807_dt_ids);
#endif

static struct i2c_driver rda5807_i2c_driver = {
	.probe = rda5807_i2c_probe,
	.remove = rda5807_i2c_remove,
	.driver = {
		.name	= "radio-rda5807",
		.owner	= THIS_MODULE,
#ifdef CONFIG_PM_SLEEP
		.pm	= &rda5807_pm_ops,
#endif
		.of_match_table = of_match_ptr(rda5807_dt_ids),
	},
};
module_i2c_driver(rda5807_i2c_driver);

MODULE_AUTHOR("Maarten ter Huurne <maarten@treewalker.org>");
MODULE_DESCRIPTION("RDA5807 FM tuner driver");
MODULE_LICENSE("GPL");
