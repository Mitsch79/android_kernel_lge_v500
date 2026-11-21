/* SPDX-License-Identifier: GPL-2.0-only */
/* AWIFI Synopsys 28 nm PHY. The comparator sequence and settling times
 * follow LG's msm_otg.c. Run before enabling the peripheral pull-up.
 */
#include <linux/ulpi/regs.h>

static enum power_supply_usb_type v500_detect_source(struct ci_hdrc *ci)
{
	const struct ulpi_ops *ops = &ci->ulpi_ops;
	struct device *dev = ci->dev;
	const u8 control = 0x84, set = 0x85, clear = 0x86, status = 0x87;
	int func, otg, saved, alt_enable, i, ret;
	enum power_supply_usb_type source = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	int contact = -1, primary = -1, secondary = -1, lines = -1;

	if (!ops->read || !ops->write)
		return source;
	func = ops->read(dev, ULPI_FUNC_CTRL);
	otg = ops->read(dev, ULPI_OTG_CTRL);
	saved = ops->read(dev, control);
	alt_enable = ops->read(dev, 0x93);
	if (func < 0 || otg < 0 || saved < 0 || alt_enable < 0)
		return source;
	ret = ops->write(dev, ULPI_FUNC_CTRL, (func & ~ULPI_FUNC_CTRL_OPMODE_MASK) |
			 ULPI_FUNC_CTRL_OPMODE_NONDRIVING);
	if (ret || ops->write(dev, ULPI_CLR(ULPI_OTG_CTRL),
			     ULPI_OTG_CTRL_DP_PULLDOWN | ULPI_OTG_CTRL_DM_PULLDOWN) ||
	    ops->write(dev, clear, 0x1f) || ops->write(dev, 0x92, 0x1f) ||
	    ops->write(dev, 0x95, 0x1f))
		goto restore;
	udelay(100);
	if (ops->write(dev, set, BIT(4)))
		goto restore;
	for (i = 0; i < 15; i++) {
		msleep(50);
		contact = ops->read(dev, status);
		if (contact < 0)
			goto restore;
		if (contact & BIT(1))
			break;
	}
	if (i == 15 || ops->write(dev, clear, BIT(4)) ||
	    ops->write(dev, set, BIT(1)) || ops->write(dev, set, BIT(0)))
		goto restore;
	msleep(50);
	primary = ops->read(dev, status);
	/* A high digital line is a proprietary signature. Keep that source
	 * unclassified rather than treating it as a BC1.2 dedicated port.
	 */
	if (ops->write(dev, clear, 0x03))
		goto restore;
	msleep(20);
	lines = hw_read(ci, OP_PORTSC, GENMASK(11, 10)) >> 10;
	if (primary < 0 || lines)
		goto restore;
	if (!(primary & BIT(0))) {
		source = POWER_SUPPLY_USB_TYPE_SDP;
		goto restore;
	}
	if (ops->write(dev, set, BIT(3)) || ops->write(dev, set, BIT(1)) ||
	    ops->write(dev, set, BIT(0)))
		goto restore;
	msleep(50);
	secondary = ops->read(dev, status);
	if (secondary >= 0)
		source = secondary & BIT(0) ? POWER_SUPPLY_USB_TYPE_DCP : POWER_SUPPLY_USB_TYPE_CDP;
restore:
	ret = ops->write(dev, control, saved);
	if (ops->write(dev, 0x93, alt_enable))
		ret = -EIO;
	if (ops->write(dev, ULPI_OTG_CTRL, otg))
		ret = -EIO;
	if (ops->write(dev, ULPI_FUNC_CTRL, func))
		ret = -EIO;
	if (ret)
		dev_warn(dev, "V500 charger detection: PHY restore failed\n");
	dev_info(dev, "V500 BC1.2: contact=%d primary=%d lines=%d secondary=%d restore=%d\n",
		 contact, primary, lines, secondary, ret);
	return ret ? POWER_SUPPLY_USB_TYPE_UNKNOWN : source;
}
