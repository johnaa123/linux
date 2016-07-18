/*
 * Copyright (C) 2016 Paul Cercueil <paul@crapouillou.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/mfd/syscon/jz4740-tcu.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define NUM_CHANNELS	8

struct ingenic_tcu;

struct ingenic_tcu_channel {
	unsigned idx;
	struct clk *clk;
};

struct ingenic_tcu {
	struct ingenic_tcu_channel channels[NUM_CHANNELS];
	unsigned long requested;
	struct regmap *map;
};

struct ingenic_clock_event_device {
	struct clock_event_device cevt;
	struct ingenic_tcu_channel *channel;
	char name[32];
};

#define ingenic_cevt(_evt) \
	container_of(_evt, struct ingenic_clock_event_device, cevt)

static inline struct ingenic_tcu *to_ingenic_tcu(struct ingenic_tcu_channel *ch)
{
	return container_of(ch, struct ingenic_tcu, channels[ch->idx]);
}

static int ingenic_tcu_cevt_set_state_shutdown(struct clock_event_device *evt)
{
	struct ingenic_clock_event_device *jzcevt = ingenic_cevt(evt);
	struct ingenic_tcu_channel *channel = jzcevt->channel;
	struct ingenic_tcu *tcu = to_ingenic_tcu(channel);
	unsigned int idx = channel->idx;

	regmap_write(tcu->map, REG_TECR, BIT(idx));
	return 0;
}

static int ingenic_tcu_cevt_set_next(unsigned long next,
		struct clock_event_device *evt)
{
	struct ingenic_clock_event_device *jzcevt = ingenic_cevt(evt);
	struct ingenic_tcu_channel *channel = jzcevt->channel;
	struct ingenic_tcu *tcu = to_ingenic_tcu(channel);
	unsigned int idx = channel->idx;

	if (next > 0xffff)
		return -EINVAL;

	regmap_write(tcu->map, REG_TDFRc(idx), (unsigned int) next);
	regmap_write(tcu->map, REG_TCNTc(idx), 0);
	regmap_write(tcu->map, REG_TESR, BIT(idx));

	return 0;
}

static irqreturn_t ingenic_tcu_cevt_cb(int irq, void *dev_id)
{
	struct clock_event_device *cevt = dev_id;
	struct ingenic_clock_event_device *jzcevt = ingenic_cevt(cevt);
	struct ingenic_tcu_channel *channel = jzcevt->channel;
	struct ingenic_tcu *tcu = to_ingenic_tcu(channel);
	unsigned int idx = channel->idx;

	regmap_write(tcu->map, REG_TECR, BIT(idx));

	if (cevt->event_handler)
		cevt->event_handler(cevt);

	return IRQ_HANDLED;
}

static int __init ingenic_tcu_req_channel(struct ingenic_tcu_channel *channel)
{
	struct ingenic_tcu *tcu = to_ingenic_tcu(channel);
	char buf[16];
	int err;

	if (test_and_set_bit(channel->idx, &tcu->requested))
		return -EBUSY;

	snprintf(buf, sizeof(buf), "timer%u", channel->idx);
	channel->clk = clk_get(NULL, buf);
	if (IS_ERR(channel->clk)) {
		err = PTR_ERR(channel->clk);
		goto out_release;
	}

	err = clk_prepare_enable(channel->clk);
	if (err)
		goto out_clk_put;

	return 0;

out_clk_put:
	clk_put(channel->clk);
out_release:
	clear_bit(channel->idx, &tcu->requested);
	return err;
}

static int __init ingenic_tcu_reset_channel(struct device_node *np,
		struct ingenic_tcu_channel *channel)
{
	struct ingenic_tcu *tcu = to_ingenic_tcu(channel);

	return regmap_update_bits(tcu->map, REG_TCSRc(channel->idx),
				0xffff & ~TCSR_RESERVED_BITS, 0);
}

static void __init ingenic_tcu_free_channel(struct ingenic_tcu_channel *channel)
{
	struct ingenic_tcu *tcu = to_ingenic_tcu(channel);

	clk_disable_unprepare(channel->clk);
	clk_put(channel->clk);
	clear_bit(channel->idx, &tcu->requested);
}

static const char * const ingenic_tcu_timer_names[] = {
	"TCU0", "TCU1", "TCU2", "TCU3", "TCU4", "TCU5", "TCU6", "TCU7",
};

static int __init ingenic_tcu_setup_cevt(struct device_node *np,
		struct ingenic_tcu *tcu, unsigned int idx)
{
	struct ingenic_tcu_channel *channel = &tcu->channels[idx];
	struct ingenic_clock_event_device *jzcevt;
	unsigned long rate;
	int err, virq;

	err = ingenic_tcu_req_channel(channel);
	if (err)
		return err;

	err = ingenic_tcu_reset_channel(np, channel);
	if (err)
		goto err_out_free_channel;

	rate = clk_get_rate(channel->clk);
	if (!rate) {
		err = -EINVAL;
		goto err_out_free_channel;
	}

	jzcevt = kzalloc(sizeof(*jzcevt), GFP_KERNEL);
	if (!jzcevt) {
		err = -ENOMEM;
		goto err_out_free_channel;
	}

	virq = irq_of_parse_and_map(np, idx);
	if (!virq) {
		err = -EINVAL;
		goto err_out_kfree_jzcevt;
	}

	err = request_irq(virq, ingenic_tcu_cevt_cb, IRQF_TIMER,
			ingenic_tcu_timer_names[idx], &jzcevt->cevt);
	if (err)
		goto err_out_irq_dispose_mapping;

	jzcevt->channel = channel;
	snprintf(jzcevt->name, sizeof(jzcevt->name), "ingenic-tcu-chan%u",
		 channel->idx);

	jzcevt->cevt.cpumask = cpumask_of(smp_processor_id());
	jzcevt->cevt.features = CLOCK_EVT_FEAT_ONESHOT;
	jzcevt->cevt.name = jzcevt->name;
	jzcevt->cevt.rating = 200;
	jzcevt->cevt.set_state_shutdown = ingenic_tcu_cevt_set_state_shutdown;
	jzcevt->cevt.set_next_event = ingenic_tcu_cevt_set_next;

	clockevents_config_and_register(&jzcevt->cevt, rate, 10, (1 << 16) - 1);

	return 0;

err_out_irq_dispose_mapping:
	irq_dispose_mapping(virq);
err_out_kfree_jzcevt:
	kfree(jzcevt);
err_out_free_channel:
	ingenic_tcu_free_channel(channel);
	return err;
}

static int __init ingenic_tcu_init(struct device_node *np)
{
	struct ingenic_tcu *tcu;
	unsigned int i;
	int err, num_timers;

	num_timers = of_property_count_elems_of_size(np, "timers", 4);
	if (num_timers < 0) {
		pr_err("timer-ingenic: Unable to read DTS node");
		return num_timers;
	}

	tcu = kzalloc(sizeof(*tcu), GFP_KERNEL);
	if (!tcu)
		return -ENOMEM;

	tcu->map = syscon_node_to_regmap(np->parent);
	if (IS_ERR(tcu->map)) {
		err = PTR_ERR(tcu->map);
		kfree(tcu);
		return err;
	}

	for (i = 0; i < NUM_CHANNELS; i++)
		tcu->channels[i].idx = i;

	for (i = 0; i < (unsigned) num_timers; i++) {
		u32 channel;

		of_property_read_u32_index(np, "timers", i, &channel);

		if (channel > NUM_CHANNELS) {
			pr_warn("timer-ingenic: requested TCU channel %u does not exist\n",
					channel);
			continue;
		}

		err = ingenic_tcu_setup_cevt(np, tcu, channel);
		if (err) {
			pr_warn("timer-ingenic: Unable to init TCU channel %u: %i",
					channel, err);
			continue;
		}
	}

	return 0;
}

CLOCKSOURCE_OF_DECLARE(jz4740_tcu, "ingenic,jz4740-tcu", ingenic_tcu_init);
CLOCKSOURCE_OF_DECLARE(jz4770_tcu, "ingenic,jz4770-tcu", ingenic_tcu_init);
CLOCKSOURCE_OF_DECLARE(jz4780_tcu, "ingenic,jz4780-tcu", ingenic_tcu_init);
