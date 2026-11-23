/* SPDX-License-Identifier: GPL-2.0-only */
/* APQ8064 SAW2 v1.1, V500 PM8921/PM8821 FTSMPS band 2. */
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/iopoll.h>
#include <linux/regulator/driver.h>

struct v500_saw {
	void __iomem *base;
	unsigned int cpu;
};

static int v500_saw_get_sel(struct regulator_dev *rdev)
{
	struct v500_saw *saw = rdev_get_drvdata(rdev);
	void __iomem *base = saw->base;
	u32 level = readl(base + 0x10) & 0xff;

	/* 850 mV to 1300 mV in steps of 12.5 mV. */
	if (level < 0x8c || level > 0xb0)
		return -ERANGE;
	return level - 0x8c;
}

static int v500_saw_set_local(struct regulator_dev *rdev, unsigned int sel)
{
	struct v500_saw *saw = rdev_get_drvdata(rdev);
	void __iomem *base = saw->base;
	u32 level = sel + 0x8c, vctl, data0, data1, sts;
	int old;

	if (sel >= 37)
		return -EINVAL;
	if (!(readl(base + 0x04) & BIT(18)) || (readl(base + 0x18) & BIT(27)))
		return -EOPNOTSUPP;
	old = v500_saw_get_sel(rdev);
	if (old < 0)
		return old;
	vctl = readl(base + 0x14);
	data0 = readl(base + 0x28);
	data1 = readl(base + 0x2c);
	writel(1, base + 0x30);
	writel((vctl & ~0xff) | level, base + 0x14);
	writel((data0 & ~0xff) | level, base + 0x28);
	writel((data1 & ~0x003f003f) | ((level & 0x3f) * 0x10001), base + 0x2c);
	if (readl_poll_timeout_atomic(base + 0x10, sts, (sts & 0xff) == level, 1, 50))
		return -ETIMEDOUT;
	if (sel > old)
		udelay((sel - old) * 10);
	return 0;
}

struct v500_saw_request {
	struct regulator_dev *rdev;
	unsigned int sel;
	int result;
};

static void v500_saw_apply(void *data)
{
	struct v500_saw_request *request = data;

	request->result = v500_saw_set_local(request->rdev, request->sel);
}

static int v500_saw_set_sel(struct regulator_dev *rdev, unsigned int sel)
{
	struct v500_saw *saw = rdev_get_drvdata(rdev);
	struct v500_saw_request request = { .rdev = rdev, .sel = sel };
	int ret;

	/* The target must be awake throughout its SAW transaction. If it is
	 * offline, propagate the IPI error and let the caller retry later.
	 */
	ret = smp_call_function_single(saw->cpu, v500_saw_apply, &request, true);
	return ret ? ret : request.result;
}

static const struct regulator_ops v500_saw_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = v500_saw_get_sel,
	.set_voltage_sel = v500_saw_set_sel,
};

static const struct regulator_desc v500_saw_desc = {
	.name = "v500-cpu-supply",
	.of_match = "cpu-regulator",
	.ops = &v500_saw_ops,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.min_uV = 850000,
	.uV_step = 12500,
	.n_voltages = 37,
};

static int v500_saw_register(struct platform_device *pdev, void __iomem *base)
{
	struct regulator_config config = { .dev = &pdev->dev };
	struct v500_saw *saw;
	int cpu;
	struct regulator_dev *rdev;

	saw = devm_kzalloc(&pdev->dev, sizeof(*saw), GFP_KERNEL);
	if (!saw)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		struct device_node *node = of_get_cpu_node(cpu, NULL);
		struct device_node *reg = of_parse_phandle(node, "cpu-supply", 0);
		bool match = reg && reg->parent == pdev->dev.of_node;

		of_node_put(reg);
		of_node_put(node);
		if (match)
			break;
	}
	if (cpu >= nr_cpu_ids)
		return dev_err_probe(&pdev->dev, -ENODEV, "missing CPU supply consumer\n");
	saw->cpu = cpu;
	saw->base = base;
	config.driver_data = saw;
	rdev = devm_regulator_register(&pdev->dev, &v500_saw_desc, &config);
	if (IS_ERR(rdev))
		return PTR_ERR(rdev);
	dev_info(&pdev->dev, "V500 CPU%d regulator registered, PMIC level %#x\n", cpu,
		 readl(base + 0x10) & 0xff);
	return 0;
}
