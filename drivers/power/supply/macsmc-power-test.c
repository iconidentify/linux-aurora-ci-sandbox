// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by macsmc-power.c; the charge-behaviour reader is stubbed. */
#include <kunit/test.h>

struct macsmc_power_test_context {
	struct macsmc_power power;
	int result;
};

static int macsmc_power_test_read_chte(struct macsmc_power *power, u8 *buf)
{
	struct macsmc_power_test_context *ctx =
		container_of(power, struct macsmc_power_test_context, power);

	memset(buf, 0xa5, 4);
	buf[0] = 1;
	return ctx->result;
}

static int macsmc_power_test_init(struct kunit *test)
{
	struct macsmc_power_test_context *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->power.has_chte = true;
	test->priv = ctx;
	kunit_activate_static_stub(test, macsmc_power_read_chte,
				   macsmc_power_test_read_chte);
	return 0;
}

static void macsmc_power_short_chte_test(struct kunit *test)
{
	struct macsmc_power_test_context *ctx = test->priv;

	ctx->result = 0;
	KUNIT_EXPECT_EQ(test, macsmc_battery_get_charge_behaviour(&ctx->power), -EIO);
	ctx->result = 2;
	KUNIT_EXPECT_EQ(test, macsmc_battery_get_charge_behaviour(&ctx->power), -EIO);
	ctx->result = 4;
	KUNIT_EXPECT_EQ(test, macsmc_battery_get_charge_behaviour(&ctx->power),
			POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE);
}

static struct kunit_case macsmc_power_test_cases[] = {
	KUNIT_CASE(macsmc_power_short_chte_test),
	{}
};

static struct kunit_suite macsmc_power_test_suite = {
	.name = "macsmc-power",
	.init = macsmc_power_test_init,
	.test_cases = macsmc_power_test_cases,
};

kunit_test_suite(macsmc_power_test_suite);
