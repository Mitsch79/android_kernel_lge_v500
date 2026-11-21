// SPDX-License-Identifier: GPL-2.0-only
/*
 * LG G Pad 8.3 (AWIFI), PM8921. Board data and register encodings from
 * LineageOS/android_kernel_lge_v500, 386aad263c38, board-awifi-pmic.c,
 * pm8921-{charger,bms}.c and pm8xxx-ccadc.c.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iio/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power/v500-battery.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>
#include "v500-battery-data.h"

#define CHG_CTRL 0x204
#define CHG_TEST 0x206
#define CHG_CTRL3 0x216
#define USB_SUSPEND BIT(2)
#define AUTO_CHARGE BIT(7)
#define USB_LIMIT_REG 0x005
#define USB_LIMIT_MASK GENMASK(4, 2)
#define BMS_CTRL 0x224
#define LG_SHUTDOWN_SOC 0x107
#define HOLD_DATA BIT(1)
#define OUTPUT_SEL GENMASK(4, 2)
#define PACK_UAH 4600000
#define POLL_MS 2000

enum { USB, PRESENT, TEMP_OK, CHARGER_HOT, OVERVOLTAGE, FAILED,
       FAST, TRICKLE, DONE, ATC_FAILED, IRQ_COUNT };
static const char * const irq_names[] = {
	"usb", "present", "temp-ok", "charger-hot", "overvoltage", "failed",
	"fast", "trickle", "done", "atc-failed"
};

struct v500_reading {
	bool lines[IRQ_COUNT];
	int temp, uv, ua, uah, capacity, health, status, limit_ua, usb_type;
};

struct v500_battery {
	struct device *dev;
	struct regmap *map;
	struct iio_channel *therm, *vbat, *ref625, *ref1250;
	struct power_supply *battery, *usb;
	struct delayed_work work;
	struct mutex lock;
	struct v500_reading sample;
	unsigned long stamp;
	int irq[IRQ_COUNT], gain_codes, ref_low, ref_high, error;
	int saved_percent, initial_offset_uah, ocv_base_uah;
	int usb_trim_base, programmed_ma;
	bool usb_trim_table1;
	u16 last_ocv;
	bool active, stopped, voltage_fault;
};

/* ChipIdea can call vbus_draw from atomic context. Serialize registration
 * and scheduling only. All SSBI and IIO transactions run on the workqueue.
 */
static DEFINE_SPINLOCK(budget_lock);
static struct v500_battery *controller;
static unsigned int budget_ma;
static enum power_supply_usb_type detected_source;

void v500_usb_set_source(enum power_supply_usb_type source)
{
	unsigned long flags;

	spin_lock_irqsave(&budget_lock, flags);
	WRITE_ONCE(detected_source, source);
	if (controller && !controller->stopped)
		mod_delayed_work(system_wq, &controller->work, 0);
	spin_unlock_irqrestore(&budget_lock, flags);
}
EXPORT_SYMBOL_GPL(v500_usb_set_source);

int v500_usb_set_budget(unsigned int ma)
{
	unsigned long flags;

	spin_lock_irqsave(&budget_lock, flags);
	WRITE_ONCE(budget_ma, min(ma, 500U));
	if (controller && !controller->stopped)
		mod_delayed_work(system_wq, &controller->work, 0);
	spin_unlock_irqrestore(&budget_lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(v500_usb_set_budget);

static int checked_bits(struct regmap *map, unsigned int reg, u8 mask, u8 value)
{
	unsigned int actual;
	int ret = regmap_update_bits(map, reg, mask, value);

	if (!ret)
		ret = regmap_read(map, reg, &actual);
	if (!ret && (actual & mask) != value)
		ret = -EIO;
	return ret;
}

static int inhibit(struct v500_battery *b)
{
	b->programmed_ma = 0;
	return checked_bits(b->map, CHG_CTRL3, AUTO_CHARGE | USB_SUSPEND, USB_SUSPEND);
}

/* LG reads the factory calibration and selects one of two PM8921 trim
 * tables. The coarse current limit alone does not apply that calibration.
 */
static int read_usb_trim(struct v500_battery *b)
{
	unsigned int hi, lo, page, select;
	int ret, restore;

	ret = regmap_read(b->map, 0x09c, &hi);
	if (!ret)
		ret = regmap_read(b->map, 0x10a, &lo);
	if (!ret)
		ret = regmap_read(b->map, 0x04f, &page);
	if (ret)
		return ret;
	ret = regmap_write(b->map, 0x04f, 0x5e);
	if (!ret)
		ret = regmap_read(b->map, 0x339, &select);
	restore = regmap_write(b->map, 0x04f, page);
	if (ret || restore)
		return ret ?: restore;
	b->usb_trim_base = (hi >> 5) << 3 | (lo >> 5);
	b->usb_trim_table1 = select & BIT(6);
	return 0;
}

static const struct v500_usb_step {
	int ma;
	u8 coarse, trim1, trim2;
} usb_steps[] = {
	{ 100, 0, 0, 0 }, { 500, 1, 5, 2 }, { 900, 4, 10, 5 },
	{ 1300, 6, 15, 6 }, { 1500, 7, 16, 6 },
};

static int set_input_limit(struct v500_battery *b, int ma)
{
	const struct v500_usb_step *step = &usb_steps[0];
	unsigned int page;
	int trim, ret, restore, i;

	for (i = 1; i < ARRAY_SIZE(usb_steps) && ma >= usb_steps[i].ma; i++) {
		step = &usb_steps[i];
		/* Raise a CDP in steps over consecutive healthy samples. */
		if (step->ma > b->programmed_ma)
			break;
	}
	if (b->programmed_ma == step->ma)
		return 0;
	ret = checked_bits(b->map, USB_LIMIT_REG, USB_LIMIT_MASK, step->coarse << 2);
	if (!ret)
		ret = regmap_read(b->map, 0x04f, &page);
	if (ret)
		return ret;
	trim = max(0, b->usb_trim_base - (b->usb_trim_table1 ? step->trim1 : step->trim2));
	ret = regmap_write(b->map, 0x04f, page | 0x06);
	if (!ret)
		ret = checked_bits(b->map, 0x355, 0x3f, trim);
	restore = regmap_write(b->map, 0x04f, page);
	if (ret || restore)
		return ret ?: restore;
	b->programmed_ma = step->ma;
	dev_info(b->dev, "USB input limit read back: %d mA\n", step->ma);
	return 0;
}

static int start_pmic(struct v500_battery *b)
{
	/* LG votes for TCXO D0 before switching the charger register clock. */
	static const struct reg_sequence clock_setup[] = {
		{ CHG_TEST, 0xd1 }, { CHG_TEST, 0xd3 }, { CHG_TEST, 0xd1 },
		{ CHG_TEST, 0xd5, 183 }, { CHG_TEST, 0xd1 }, { CHG_TEST, 0xd0, 32 },
		{ CHG_TEST, 0xd1 }, { CHG_TEST, 0xd3 }
	};
	static const struct { u16 reg; u8 mask, value; } settings[] = {
		{ 0x212, 0x07, 0x02 }, /* enable HW temperature check, AWIFI thresholds */
		/* AWIFI's pack is rated for 4.32 V. Its independent safety
		 * comparator also sees a full battery carried over from 14.1.
		 * Regulate below that limit at 4.20 V for these USB tests.
		 */
		{ 0x221, 0x7f, (4320 - 3240) / 20 },
		{ 0x220, 0xff, (4200 - 3240) / 20 }, /* regulation voltage */
		{ 0x213, 0x7f, (4140 - 3240) / 20 }, /* recharge threshold */
		{ 0x210, 0x3f, (475 - 225) / 50 }, /* battery safety current */
		{ 0x205, 0x3f, (475 - 225) / 50 }, /* fast-charge current */
		{ 0x215, 0x0f, (200 - 50) / 10 }, /* termination current */
		{ 0x217, 0x1f, (4400 - 3800) / 100 }, /* USB voltage regulation */
		{ 0x21b, 0x7f, (180 - 1) / 4 }, /* hardware safety timer */
	};
	unsigned int ctrl, stored;
	int i, ret;

	msleep(4);
	ret = regmap_multi_reg_write(b->map, clock_setup, ARRAY_SIZE(clock_setup));
	if (!ret)
		ret = inhibit(b);
	/* LG's battery-backed scratch is valid across a warm restart with
	 * the same pack. VREF is cleared by hardware on battery removal.
	 */
	b->saved_percent = -1;
	if (!ret && !regmap_read(b->map, CHG_CTRL, &ctrl) && (ctrl & BIT(7)) &&
	    !regmap_read(b->map, LG_SHUTDOWN_SOC, &stored)) {
		if (stored == 0xff)
			b->saved_percent = 0;
		else if (stored > 0 && stored <= 100)
			b->saved_percent = stored;
	}
	if (!ret)
		ret = checked_bits(b->map, CHG_CTRL, BIT(7), BIT(7));
	if (!ret)
		ret = checked_bits(b->map, BMS_CTRL, BIT(7) | BIT(0), BIT(7) | BIT(0));
	/* Measurement-only boots leave the existing pack thresholds intact. */
	if (ret || !b->active)
		return ret;
	ret = checked_bits(b->map, USB_LIMIT_REG, USB_LIMIT_MASK, 0);
	if (!ret)
		ret = checked_bits(b->map, 0x007, BIT(6), BIT(6));
	for (i = 0; !ret && i < ARRAY_SIZE(settings); i++)
		ret = checked_bits(b->map, settings[i].reg, settings[i].mask, settings[i].value);
	if (!ret)
		ret = read_usb_trim(b);
	return ret;
}

/* Full-scale and offset trims share the PMIC page selector. Restore the
 * original selector on every path after enabling the trim page.
 */
static int read_gain(struct v500_battery *b)
{
	unsigned int saved, v[4];
	int i, ret, restore;

	ret = regmap_read(b->map, 0x04f, &saved);
	if (ret)
		return ret;
	ret = regmap_update_bits(b->map, 0x04f, 0x06, 0x06);
	for (i = 0; !ret && i < ARRAY_SIZE(v); i++)
		ret = regmap_read(b->map, 0x34a + i, &v[i]);
	restore = regmap_update_bits(b->map, 0x04f, 0x06, saved & 0x06);
	if (ret || restore)
		return ret ?: restore;
	b->gain_codes = (v[2] * 256 + v[3]) - (v[0] * 256 + v[1]);
	/* Reference driver accepts 20 to 30 mV for the nominal 25 mV trim. */
	if (b->gain_codes * 542535LL < 20000LL * 100000 ||
	    b->gain_codes * 542535LL > 30000LL * 100000)
		return -ERANGE;
	return 0;
}

static int battery_temp(struct v500_battery *b, int *temp)
{
	int i, mv, ret = iio_read_channel_processed(b->therm, &mv);

	if (ret < 0)
		return ret;
	if (mv > thermistor_mv[0] || mv < thermistor_mv[ARRAY_SIZE(thermistor_mv) - 1])
		return -ERANGE;
	for (i = 1; i < ARRAY_SIZE(thermistor_mv); i++) {
		if (mv >= thermistor_mv[i]) {
			*temp = (i - 31) * 10 + (thermistor_mv[i - 1] - mv) * 10 /
				(thermistor_mv[i - 1] - thermistor_mv[i]);
			return 0;
		}
	}
	return -ERANGE;
}

static int ocv_capacity(int mv, int temp)
{
	static const int temperatures[] = { -200, 0, 250, 400, 600 };
	int i, col = 1, prev = 0;

	temp = clamp(temp, -200, 600);
	while (col < ARRAY_SIZE(temperatures) - 1 && temp > temperatures[col])
		col++;
	for (i = 0; i < ARRAY_SIZE(ocv_table); i++) {
		const int *row = ocv_table[i].mv;
		int voltage = row[col - 1] + (row[col] - row[col - 1]) *
			(temp - temperatures[col - 1]) / (temperatures[col] - temperatures[col - 1]);
		if (mv >= voltage) {
			int pc = ocv_table[i].percent * 1000;
			if (!i)
				return pc;
			return pc + (mv - voltage) * (ocv_table[i - 1].percent * 1000 - pc) /
				(prev - voltage);
		}
		prev = voltage;
	}
	return 0;
}

static int read_word(struct v500_battery *b, unsigned int mux, u16 *word)
{
	unsigned int lo, hi;
	int ret = regmap_update_bits(b->map, BMS_CTRL, OUTPUT_SEL, mux << 2);

	if (!ret)
		ret = regmap_read(b->map, 0x230, &lo);
	if (!ret)
		ret = regmap_read(b->map, 0x231, &hi);
	if (!ret)
		*word = (hi << 8) | lo;
	return ret;
}

static int read_gauge(struct v500_battery *b, struct v500_reading *r)
{
	u16 ocv, sense, hi, lo;
	int ocv_mv, ret, release;
	s64 cc_uv, cc_uah, remaining;

	ret = regmap_update_bits(b->map, BMS_CTRL, HOLD_DATA, HOLD_DATA);
	if (ret)
		return ret;
	ret = read_word(b, 5, &ocv);
	if (!ret)
		ret = read_word(b, 6, &sense);
	if (!ret)
		ret = read_word(b, 3, &hi);
	if (!ret)
		ret = read_word(b, 4, &lo);
	release = regmap_update_bits(b->map, BMS_CTRL, HOLD_DATA, 0);
	if (ret || release)
		return ret ?: release;
	ocv_mv = 1875 + div_s64(1875LL * (ocv - b->ref_low), b->ref_high - b->ref_low);
	if (ocv_mv < 2500 || ocv_mv > 4400)
		return -ERANGE;
	/* Positive current and charge counter mean charge into the cell.
	 * The PM8921 itself counts discharge positive across the 10 mOhm shunt.
	 */
	r->ua = div_s64(-(s64)(s16)sense * 2500000, b->gain_codes);
	cc_uv = div_s64((s64)(s32)((u32)hi << 16 | lo) * 868056, 10000);
	cc_uv = div_s64(cc_uv * 25000, div_s64(b->gain_codes * 542535LL, 100000));
	cc_uah = div_s64(cc_uv * 56 * 100, 32764 * 3600);
	/* Keep the temperature associated with a given rest-voltage event.
	 * Cooling during a display-off interval is not a coulomb transfer.
	 */
	if (b->last_ocv != ocv) {
		b->ocv_base_uah = div_s64((s64)ocv_capacity(ocv_mv, r->temp) * PACK_UAH, 100000);
		b->initial_offset_uah = 0;
		b->last_ocv = ocv;
	}
	remaining = b->ocv_base_uah - cc_uah;
	if (remaining < -PACK_UAH / 10 || remaining > PACK_UAH * 11 / 10)
		return -ERANGE;
	if (b->saved_percent >= 0) {
		int delta = b->saved_percent * (PACK_UAH / 100) - remaining;

		/* AWIFI accepts the last saved SOC within 25 percentage points.
		 * The PMIC may still have the OCV measured before a load test.
		 */
		if (abs(delta) <= PACK_UAH / 4)
			b->initial_offset_uah = delta;
		dev_info(b->dev, "boot gauge: LG saved=%d%% OCV=%d mV estimate=%lld uAh offset=%d uAh\n",
			 b->saved_percent, ocv_mv, remaining, b->initial_offset_uah);
		b->saved_percent = -1;
	}
	remaining += b->initial_offset_uah;
	r->uah = clamp_t(s64, remaining, 0, PACK_UAH);
	/* Match the normal (non-charger-logo) LG Android display scale. */
	r->capacity = min(100, DIV_ROUND_CLOSEST(r->uah, PACK_UAH / 100) * 104 / 101);
	return 0;
}

static int sample_pack(struct v500_battery *b, struct v500_reading *r)
{
	int i, ret;

	for (i = 0; i < IRQ_COUNT; i++) {
		ret = irq_get_irqchip_state(b->irq[i], IRQCHIP_STATE_LINE_LEVEL, &r->lines[i]);
		if (ret)
			return ret;
	}
	if (!r->lines[PRESENT])
		return -ENODEV;
	ret = battery_temp(b, &r->temp);
	if (ret)
		return ret;
	ret = iio_read_channel_processed(b->vbat, &r->uv);
	if (ret < 0)
		return ret;
	if (r->uv < 2500000 || r->uv > 4500000)
		return -ERANGE;
	ret = read_gauge(b, r);
	if (ret)
		return ret;
	if (r->lines[OVERVOLTAGE] || r->uv > 4320000)
		r->health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	else if (r->lines[CHARGER_HOT] || r->temp >= 420)
		r->health = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (r->temp < 100)
		r->health = POWER_SUPPLY_HEALTH_COLD;
	else if (!r->lines[TEMP_OK] || r->lines[FAILED] || r->lines[ATC_FAILED])
		r->health = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
	else
		r->health = POWER_SUPPLY_HEALTH_GOOD;
	return 0;
}

static int apply_budget(struct v500_battery *b, struct v500_reading *r)
{
	enum power_supply_usb_type source = READ_ONCE(detected_source);
	unsigned int ma = READ_ONCE(budget_ma);
	int ret;

	/* Keep DCP at its previously tested limit. For CDP require the
	 * electrical signature and the configured gadget power request.
	 * Honor zero/low-power requests on suspend or disconnect.
	 */
	if (source == POWER_SUPPLY_USB_TYPE_DCP)
		ma = 500;
	else if (source == POWER_SUPPLY_USB_TYPE_CDP && ma >= 500)
		ma = 1500;
	r->usb_type = r->lines[USB] ? source : POWER_SUPPLY_USB_TYPE_UNKNOWN;
	if (r->lines[USB] && r->usb_type == POWER_SUPPLY_USB_TYPE_UNKNOWN && ma >= 100)
		r->usb_type = POWER_SUPPLY_USB_TYPE_SDP;
	/* A load-dependent VBAT_OV can clear as soon as USB is suspended.
	 * Do not retry a hardware overvoltage every polling interval.
	 * Clear the interlock only after observing physical VBUS removal.
	 */
	if (!r->lines[USB])
		b->voltage_fault = false;
	else if (r->health == POWER_SUPPLY_HEALTH_OVERVOLTAGE)
		b->voltage_fault = true;
	if (b->active && b->voltage_fault && r->health == POWER_SUPPLY_HEALTH_GOOD)
		r->health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	if (!b->active || ma < 100 || !r->lines[USB] ||
	    r->health != POWER_SUPPLY_HEALTH_GOOD || r->uv < 3200000)
		return inhibit(b);
	ret = set_input_limit(b, ma);
	if (!ret)
		ret = checked_bits(b->map, CHG_CTRL3, AUTO_CHARGE | USB_SUSPEND, AUTO_CHARGE);
	if (!ret)
		r->limit_ua = b->programmed_ma * 1000;
	return ret;
}

static void sample_work(struct work_struct *work)
{
	struct v500_battery *b = container_of(to_delayed_work(work), struct v500_battery, work);
	struct v500_reading r = {};
	int ret;

	if (READ_ONCE(detected_source) != POWER_SUPPLY_USB_TYPE_DCP && READ_ONCE(budget_ma) < 100)
		inhibit(b);
	ret = sample_pack(b, &r);
	if (!ret)
		ret = apply_budget(b, &r);
	if (ret) {
		if (inhibit(b))
			dev_err_ratelimited(b->dev, "Cannot confirm charger inhibited\n");
		dev_warn_ratelimited(b->dev, "PM8921 battery sample/control: %d\n", ret);
	} else if (!r.lines[USB]) {
		r.status = POWER_SUPPLY_STATUS_DISCHARGING;
	} else if (r.limit_ua && r.lines[DONE]) {
		r.status = POWER_SUPPLY_STATUS_FULL;
	} else if (r.limit_ua && (r.lines[FAST] || r.lines[TRICKLE]) && r.ua > 0) {
		r.status = POWER_SUPPLY_STATUS_CHARGING;
	} else {
		r.status = POWER_SUPPLY_STATUS_NOT_CHARGING;
	}
	mutex_lock(&b->lock);
	b->error = ret;
	b->sample = r;
	b->stamp = jiffies;
	mutex_unlock(&b->lock);
	power_supply_changed(b->battery);
	power_supply_changed(b->usb);
	schedule_delayed_work(&b->work, msecs_to_jiffies(POLL_MS));
}

static enum power_supply_property battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT, POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_TEMP, POWER_SUPPLY_PROP_VOLTAGE_NOW, POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY, POWER_SUPPLY_PROP_CHARGE_COUNTER, POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
};
static enum power_supply_property usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE, POWER_SUPPLY_PROP_CURRENT_MAX, POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_USB_TYPE,
};
static enum power_supply_usb_type usb_types[] = {
	POWER_SUPPLY_USB_TYPE_UNKNOWN, POWER_SUPPLY_USB_TYPE_SDP, POWER_SUPPLY_USB_TYPE_DCP,
	POWER_SUPPLY_USB_TYPE_CDP,
};

static int get_property(struct power_supply *psy, enum power_supply_property prop,
			union power_supply_propval *val)
{
	struct v500_battery *b = power_supply_get_drvdata(psy);
	struct v500_reading r;
	int ret;

	mutex_lock(&b->lock);
	r = b->sample;
	ret = b->error;
	if (time_after(jiffies, b->stamp + msecs_to_jiffies(3 * POLL_MS)))
		ret = -ENODATA;
	mutex_unlock(&b->lock);
	if (ret) {
		if (ret == -ENODEV && prop == POWER_SUPPLY_PROP_PRESENT) {
			val->intval = 0;
			return 0;
		}
		return -ENODATA;
	}
	switch (prop) {
	case POWER_SUPPLY_PROP_PRESENT: val->intval = r.lines[PRESENT]; break;
	case POWER_SUPPLY_PROP_ONLINE: val->intval = r.lines[USB]; break;
	case POWER_SUPPLY_PROP_USB_TYPE: val->intval = r.usb_type; break;
	case POWER_SUPPLY_PROP_CURRENT_MAX: val->intval = r.limit_ua; break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX: val->intval = 5000000; break;
	case POWER_SUPPLY_PROP_STATUS: val->intval = r.status; break;
	case POWER_SUPPLY_PROP_HEALTH: val->intval = r.health; break;
	case POWER_SUPPLY_PROP_TEMP: val->intval = r.temp; break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW: val->intval = r.uv; break;
	case POWER_SUPPLY_PROP_CURRENT_NOW: val->intval = r.ua; break;
	case POWER_SUPPLY_PROP_CAPACITY: val->intval = r.capacity; break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER: val->intval = r.uah; break;
	case POWER_SUPPLY_PROP_TECHNOLOGY: val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO; break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN: val->intval = PACK_UAH; break;
	default: return -EINVAL;
	}
	return 0;
}

static const struct power_supply_desc battery_desc = {
	.name = "battery", .type = POWER_SUPPLY_TYPE_BATTERY, .properties = battery_props,
	.num_properties = ARRAY_SIZE(battery_props), .get_property = get_property,
};
static const struct power_supply_desc usb_desc = {
	.name = "usb", .type = POWER_SUPPLY_TYPE_USB, .properties = usb_props,
	.usb_types = usb_types, .num_usb_types = ARRAY_SIZE(usb_types),
	.num_properties = ARRAY_SIZE(usb_props), .get_property = get_property,
};

static int pause_charger(struct device *dev)
{
	struct v500_battery *b = dev_get_drvdata(dev);
	unsigned long flags;

	spin_lock_irqsave(&budget_lock, flags);
	b->stopped = true;
	spin_unlock_irqrestore(&budget_lock, flags);
	cancel_delayed_work_sync(&b->work);
	mutex_lock(&b->lock);
	b->error = -ENODATA;
	mutex_unlock(&b->lock);
	return inhibit(b);
}

static int resume_charger(struct device *dev)
{
	struct v500_battery *b = dev_get_drvdata(dev);
	unsigned long flags;

	spin_lock_irqsave(&budget_lock, flags);
	b->stopped = false;
	mod_delayed_work(system_wq, &b->work, 0);
	spin_unlock_irqrestore(&budget_lock, flags);
	return 0;
}

static int v500_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct v500_battery *b;
	struct power_supply_config cfg = {};
	struct clk *clk;
	unsigned long flags;
	int i, ret;

	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev;
	b->map = dev_get_regmap(dev->parent, NULL);
	if (!b->map)
		return -ENODEV;
	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "charger TCXO\n");
	b->active = of_property_read_bool(dev->of_node, "lge,enable-usb-charging");
	ret = start_pmic(b);
	if (ret)
		return dev_err_probe(dev, ret, "charger initialization\n");
	b->therm = devm_iio_channel_get(dev, "thermistor");
	if (IS_ERR(b->therm))
		return dev_err_probe(dev, PTR_ERR(b->therm), "thermistor\n");
	b->vbat = devm_iio_channel_get(dev, "vbat");
	if (IS_ERR(b->vbat))
		return dev_err_probe(dev, PTR_ERR(b->vbat), "cell voltage\n");
	b->ref625 = devm_iio_channel_get(dev, "ref625");
	if (IS_ERR(b->ref625))
		return dev_err_probe(dev, PTR_ERR(b->ref625), "625 mV reference\n");
	b->ref1250 = devm_iio_channel_get(dev, "ref1250");
	if (IS_ERR(b->ref1250))
		return dev_err_probe(dev, PTR_ERR(b->ref1250), "1250 mV reference\n");
	/* The BMS latches OCV until its next rest interval. Apply the same
	 * calibration throughout the boot so ADC noise cannot move that OCV.
	 */
	ret = iio_read_channel_raw(b->ref625, &b->ref_low);
	if (ret < 0)
		return ret;
	ret = iio_read_channel_raw(b->ref1250, &b->ref_high);
	if (ret < 0)
		return ret;
	if (b->ref_high <= b->ref_low)
		return -ERANGE;
	for (i = 0; i < IRQ_COUNT; i++) {
		b->irq[i] = platform_get_irq_byname(pdev, irq_names[i]);
		if (b->irq[i] < 0)
			return b->irq[i];
	}
	ret = read_gain(b);
	if (ret)
		return dev_err_probe(dev, ret, "CCADC gain trim\n");
	mutex_init(&b->lock);
	INIT_DELAYED_WORK(&b->work, sample_work);
	b->error = -ENODATA;
	cfg.drv_data = b;
	b->battery = devm_power_supply_register(dev, &battery_desc, &cfg);
	if (IS_ERR(b->battery))
		return PTR_ERR(b->battery);
	b->usb = devm_power_supply_register(dev, &usb_desc, &cfg);
	if (IS_ERR(b->usb))
		return PTR_ERR(b->usb);
	platform_set_drvdata(pdev, b);
	spin_lock_irqsave(&budget_lock, flags);
	controller = b;
	mod_delayed_work(system_wq, &b->work, 0);
	spin_unlock_irqrestore(&budget_lock, flags);
	return 0;
}

static void v500_shutdown(struct platform_device *pdev)
{
	struct v500_battery *b = platform_get_drvdata(pdev);
	unsigned int raw_soc;

	/* Only store a fresh, healthy gauge on orderly shutdown. Keep the
	 * original unscaled format consumed by 14.1, TWRP and the next boot.
	 */
	mutex_lock(&b->lock);
	if (!b->error && b->sample.health == POWER_SUPPLY_HEALTH_GOOD &&
	    time_before(jiffies, b->stamp + msecs_to_jiffies(3 * POLL_MS))) {
		raw_soc = DIV_ROUND_CLOSEST(b->sample.uah, PACK_UAH / 100);
		if (regmap_write(b->map, LG_SHUTDOWN_SOC, raw_soc ?: 0xff))
			dev_warn(b->dev, "Could not save SOC at shutdown\n");
	}
	mutex_unlock(&b->lock);
	if (pause_charger(&pdev->dev))
		dev_err(&pdev->dev, "Cannot confirm charger inhibited at shutdown\n");
}

static int v500_remove(struct platform_device *pdev)
{
	unsigned long flags;

	v500_shutdown(pdev);
	spin_lock_irqsave(&budget_lock, flags);
	controller = NULL;
	spin_unlock_irqrestore(&budget_lock, flags);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(v500_pm, pause_charger, resume_charger);
static const struct of_device_id v500_match[] = {
	{ .compatible = "lge,v500-pm8921-battery" }, {}
};
static struct platform_driver v500_driver = {
	.probe = v500_probe, .remove = v500_remove, .shutdown = v500_shutdown,
	.driver = { .name = "v500-battery", .of_match_table = v500_match,
		    .pm = pm_sleep_ptr(&v500_pm) },
};
module_platform_driver(v500_driver);
MODULE_LICENSE("GPL");
