/* SPDX-License-Identifier: GPL-2.0-only */
/* AWIFI Synopsys 28 nm PHY. The comparator sequence and settling times
 * follow LG's msm_otg.c. Run before enabling the peripheral pull-up.
 */
#include <linux/ulpi/regs.h>

static bool v500_detect_dcp(struct ci_hdrc *ci)
{
	const struct ulpi_ops *ops = &ci->ulpi_ops;
	struct device *dev = ci->dev;
	const u8 control = 0x84, set = 0x85, clear = 0x86, status = 0x87;
	int func, otg, saved, sample, i, ret;
	bool dedicated = false;

	if (!ops->read || !ops->write)
		return false;
	func = ops->read(dev, ULPI_FUNC_CTRL);
	otg = ops->read(dev, ULPI_OTG_CTRL);
	saved = ops->read(dev, control);
	if (func < 0 || otg < 0 || saved < 0)
		return false;
	ret = ops->write(dev, ULPI_FUNC_CTRL, (func & ~ULPI_FUNC_CTRL_OPMODE_MASK) |
			 ULPI_FUNC_CTRL_OPMODE_NONDRIVING);
	if (ret || ops->write(dev, ULPI_CLR(ULPI_OTG_CTRL),
			     ULPI_OTG_CTRL_DP_PULLDOWN | ULPI_OTG_CTRL_DM_PULLDOWN) ||
	    ops->write(dev, clear, 0x3f) || ops->write(dev, set, BIT(4)))
		goto restore;
	for (i = 0; i < 15; i++) {
		msleep(50);
		sample = ops->read(dev, status);
		if (sample < 0)
			goto restore;
		if (sample & BIT(1))
			break;
	}
	if (i == 15 || ops->write(dev, clear, BIT(4)) || ops->write(dev, set, 0x03))
		goto restore;
	msleep(50);
	sample = ops->read(dev, status);
	/* A high digital line is a proprietary signature. Keep that source
	 * unclassified rather than treating it as a BC1.2 dedicated port.
	 */
	if (sample < 0 || !(sample & BIT(0)) || hw_read(ci, OP_PORTSC, GENMASK(11, 10)))
		goto restore;
	if (ops->write(dev, clear, 0x03))
		goto restore;
	msleep(20);
	if (ops->write(dev, set, BIT(3)) || ops->write(dev, set, 0x03))
		goto restore;
	msleep(50);
	sample = ops->read(dev, status);
	dedicated = sample >= 0 && (sample & BIT(0));
restore:
	ret = ops->write(dev, control, saved);
	if (ops->write(dev, ULPI_OTG_CTRL, otg))
		ret = -EIO;
	if (ops->write(dev, ULPI_FUNC_CTRL, func))
		ret = -EIO;
	if (ret)
		dev_warn(dev, "V500 charger detection: PHY restore failed\n");
	return dedicated && !ret;
}
