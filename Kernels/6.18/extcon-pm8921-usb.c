// SPDX-License-Identifier: GPL-2.0-only
#include <linux/extcon-provider.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

struct pm8921_usb {
	struct device *dev;
	struct extcon_dev *extcon;
	struct mutex lock;
};

static const unsigned int pm8921_usb_cables[] = { EXTCON_USB, EXTCON_NONE };

static int pm8921_usb_update(struct pm8921_usb *usb, int irq)
{
	bool present;
	int ret;

	ret = irq_get_irqchip_state(irq, IRQCHIP_STATE_LINE_LEVEL, &present);
	if (ret)
		return ret;

	dev_info(usb->dev, "USBIN_VALID=%d\n", present);
	return extcon_set_state_sync(usb->extcon, EXTCON_USB, present);
}

static irqreturn_t pm8921_usb_irq(int irq, void *data)
{
	struct pm8921_usb *usb = data;
	int ret;

	mutex_lock(&usb->lock);
	ret = pm8921_usb_update(usb, irq);
	mutex_unlock(&usb->lock);
	if (ret)
		dev_err_ratelimited(usb->dev, "Failed to update VBUS: %d\n", ret);
	return IRQ_HANDLED;
}

static int pm8921_usb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pm8921_usb *usb;
	int irq, ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	usb = devm_kzalloc(dev, sizeof(*usb), GFP_KERNEL);
	if (!usb)
		return -ENOMEM;

	usb->dev = dev;
	mutex_init(&usb->lock);
	usb->extcon = devm_extcon_dev_allocate(dev, pm8921_usb_cables);
	if (IS_ERR(usb->extcon))
		return PTR_ERR(usb->extcon);
	ret = devm_extcon_dev_register(dev, usb->extcon);
	if (ret)
		return ret;

	mutex_lock(&usb->lock);
	ret = devm_request_threaded_irq(dev, irq, NULL, pm8921_usb_irq,
					IRQF_ONESHOT, dev_name(dev), usb);
	if (!ret)
		ret = pm8921_usb_update(usb, irq);
	mutex_unlock(&usb->lock);
	return ret;
}

static const struct of_device_id pm8921_usb_of_match[] = {
	{ .compatible = "qcom,pm8921-usb-vbus" },
	{ }
};
MODULE_DEVICE_TABLE(of, pm8921_usb_of_match);

static struct platform_driver pm8921_usb_driver = {
	.probe = pm8921_usb_probe,
	.driver = {
		.name = "pm8921-usb-vbus",
		.of_match_table = pm8921_usb_of_match,
	},
};
module_platform_driver(pm8921_usb_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm PM8921 USB VBUS extcon");
