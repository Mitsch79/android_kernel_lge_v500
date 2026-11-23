/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/regulator/consumer.h>

/* LG's L2(15) operating point. Hold shared resources across independent
 * CPU policies; CPU voltages are switched individually by cpufreq-dt.
 */
static int v500_prepare_krait(struct device *dev)
{
	static const char * const parents[] = {
		"aux0", "aux1", "aux2", "aux3", "aux-l2",
		"pll0", "pll1", "pll2", "pll3", "pll-l2",
	};
	struct regulator *mem, *dig, *pll;
	struct clk *clk;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(parents); i++) {
		clk = devm_clk_get(dev, parents[i]);
		if (IS_ERR(clk))
			return PTR_ERR(clk);
	}
	mem = devm_regulator_get_optional(dev, "mem");
	if (IS_ERR(mem))
		return PTR_ERR(mem);
	dig = devm_regulator_get_optional(dev, "dig");
	if (IS_ERR(dig))
		return PTR_ERR(dig);
	pll = devm_regulator_get_optional(dev, "pll");
	if (IS_ERR(pll))
		return PTR_ERR(pll);
	clk = devm_clk_get(dev, "ebi");
	if (IS_ERR(clk))
		return PTR_ERR(clk);
	ret = regulator_set_voltage(mem, 1150000, 1150000);
	if (ret)
		return ret;
	ret = regulator_set_voltage(dig, 1150000, 1150000);
	if (ret)
		return ret;
	ret = regulator_enable(mem);
	if (ret)
		return ret;
	ret = regulator_enable(dig);
	if (ret)
		goto disable_mem;
	ret = regulator_enable(pll);
	if (ret)
		goto disable_dig;
	ret = clk_set_min_rate(clk, 533000000);
	if (!ret)
		ret = clk_prepare_enable(clk);
	if (!ret)
		return 0;
	regulator_disable(pll);
disable_dig:
	regulator_disable(dig);
disable_mem:
	regulator_disable(mem);
	return ret;
}
