// SPDX-License-Identifier: GPL-2.0-only
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/seq_file.h>
#include <asm/krait-l2-accessors.h>

/* The four CPU HFPLLs and the following L2 HFPLL in LG's acpuclock-8064.c. */
static void __iomem *pll;
static struct dentry *snapshot;

static int core_clocks_show(struct seq_file *output, void *unused)
{
	static const unsigned int mux_addr[] = { 0x4501, 0x5501, 0x6501, 0x7501, 0x0500 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mux_addr); i++) {
		void __iomem *base = pll + i * 0x40;
		u32 mux = krait_get_l2_indirect_reg(mux_addr[i]);

		seq_printf(output, "%s%u mux=%08x mode=%08x L=%u M=%u N=%u\n",
			   i == 4 ? "L" : "cpu", i == 4 ? 2 : i, mux, readl(base),
			   readl(base + 8), readl(base + 12), readl(base + 16));
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(core_clocks);

static int __init core_clocks_init(void)
{
	if (!of_machine_is_compatible("lg,v500") || !of_machine_is_compatible("qcom,apq8064"))
		return -ENODEV;
	pll = ioremap(0x00903200, 0x114);
	if (!pll)
		return -ENOMEM;
	snapshot = debugfs_create_file("v500_core_clocks", 0400, NULL, NULL, &core_clocks_fops);
	if (IS_ERR_OR_NULL(snapshot)) {
		iounmap(pll);
		return snapshot ? PTR_ERR(snapshot) : -ENODEV;
	}
	return 0;
}
module_init(core_clocks_init);
MODULE_LICENSE("GPL");
