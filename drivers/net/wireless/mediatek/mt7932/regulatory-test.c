// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the regulatory snapshot logic and the power package
 * parser and channel filter. The packages are local files, so
 * the parser treats them as untrusted.
 */
#include <kunit/test.h>
#include <linux/prandom.h>

#include "mt7932.h"

/* A world-domain snapshot: channels 1, 6, 36 and 149, the 5 GHz ones NO_IR. */
static void reg_build(struct mt7932_reg_snapshot *reg)
{
	static const u16 channels[] = {1, 6, 36, 149};
	unsigned int i;

	memset(reg, 0, sizeof(*reg));
	memcpy(reg->alpha2, "00", 2);
	memcpy(reg->domain, "XZ", 2);
	reg->domain[4] = 1;
	reg->domain[5] = 2;
	reg->domain[8] = 2;
	reg->domain[9] = 2;
	for (i = 0; i < ARRAY_SIZE(channels); i++) {
		put_unaligned_le16(channels[i], reg->domain + 12 + i * 8);
		put_unaligned_le32(channels[i] > 14 ? IEEE80211_CHAN_NO_IR : 0,
				   reg->domain + 16 + i * 8);
		reg->power[i] = 20;
	}
	reg->length = 12 + ARRAY_SIZE(channels) * 8;
}

static void reg_set_flags(struct mt7932_reg_snapshot *reg, unsigned int entry, u32 flags)
{
	put_unaligned_le32(flags, reg->domain + 16 + entry * 8);
}

/* A beacon hint lifting NO_IR only relaxes; nothing else does. */
static void mt7932_reg_relaxation_test(struct kunit *test)
{
	struct mt7932_reg_snapshot *prev, *next;

	prev = kunit_kzalloc(test, sizeof(*prev), GFP_KERNEL);
	next = kunit_kzalloc(test, sizeof(*next), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, prev);
	KUNIT_ASSERT_NOT_NULL(test, next);
	reg_build(prev);

	reg_build(next);
	reg_set_flags(next, 2, 0);
	KUNIT_EXPECT_TRUE(test, mt7932_reg_only_relaxes(prev, next));
	/* An identical snapshot is not treated as a change by the caller,
	 * but it is trivially within the applied domain.
	 */
	KUNIT_EXPECT_TRUE(test, mt7932_reg_only_relaxes(prev, prev));

	/* NO_IR added: a restriction. */
	reg_build(next);
	reg_set_flags(next, 0, IEEE80211_CHAN_NO_IR);
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	/* Radar or a bandwidth limit appears, or another flag goes away. */
	reg_build(next);
	reg_set_flags(next, 2, IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR);
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_set_flags(prev, 3, IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40);
	reg_build(next);
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_build(prev);
	/* A new country, power limit, channel or channel order. */
	reg_build(next);
	memcpy(next->alpha2, "AU", 2);
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_build(next);
	next->power[1] = 19;
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_build(next);
	put_unaligned_le16(40, next->domain + 12 + 3 * 8);
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_build(next);
	put_unaligned_le16(44, next->domain + 12 + 4 * 8);
	next->domain[9] = 3;
	next->length += 8;
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	/* A failed snapshot never relaxes, and a corrupt length is refused. */
	reg_build(next);
	next->error = -EOPNOTSUPP;
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
	reg_build(next);
	prev->length = sizeof(prev->domain) + 8;
	next->length = prev->length;
	KUNIT_EXPECT_FALSE(test, mt7932_reg_only_relaxes(prev, next));
}

#define PKG_MODE_SIZE 328
#define PKG_ENTRY_SIZE 122

/* Channels of the synthetic 2.4 and 5 GHz tables 1..9, in table order. */
static const u8 pkg_channels[] = {
	1, 2, 3, 4, 5, 6, 7, 8,
	9, 10, 11, 12, 13, 14,
	36, 40, 44, 48, 52, 56, 60, 64,
	100, 104, 108, 112, 116, 120, 124, 128,
	132, 136, 140, 144, 149, 153, 157, 161,
	38, 46, 54, 62, 102, 110, 118, 126,
	134, 142, 151, 159, 42, 58, 106, 122,
	138, 155, 50, 114, 130, 146, 163, 167,
	165, 169, 171, 173, 175,
};

static unsigned int pkg_count(unsigned int table, bool world)
{
	if (table == 2)
		return world ? 6 : 5;
	return table == 9 ? 5 : 8;
}

static size_t pkg_size(bool world)
{
	static const u16 others[] = {532, 144, 58, 265};
	size_t size = 16 + (world ? 2 : 3) * PKG_MODE_SIZE;
	unsigned int i;

	for (i = 1; i <= 9; i++)
		size += 44 + pkg_count(i, world) * PKG_ENTRY_SIZE;
	for (i = 0; i < ARRAY_SIZE(others); i++)
		size += others[i];
	return size;
}

/* Build a structurally valid package. Channel 14 is only in the world one. */
static u8 *pkg_build(struct kunit *test, const char *alpha2, size_t *size)
{
	static const u16 lengths[] = {532, 144, 58, 265};
	static const u8 types[] = {5, 1, 4, 2};
	bool world = !memcmp(alpha2, "XZ", 2);
	unsigned int i, j, modes = world ? 2 : 3, channel = 0;
	u8 *data, *p;

	*size = pkg_size(world);
	data = kunit_kzalloc(test, *size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);
	memcpy(data, "J7RP", 4);
	put_unaligned_le16(1, data + 4);
	memcpy(data + 6, alpha2, 2);
	data[8] = modes;
	data[9] = MT7932_POLICY_TABLES;
	put_unaligned_le32(*size, data + 12);
	p = data + 16;
	for (i = 0; i < modes; i++, p += PKG_MODE_SIZE)
		put_unaligned_le32(17 + i, p + 4);
	for (i = 0; i < MT7932_POLICY_TABLES; i++) {
		if (i >= 1 && i <= 9) {
			unsigned int count = pkg_count(i, world);

			p[0] = 3;
			p[1] = 0x0a;
			put_unaligned_le16(44 + count * PKG_ENTRY_SIZE, p + 2);
			p[4] = count;
			p[5] = i <= 2 ? 1 : 2;
			p[6] = i == 2 || i == 9;
			memcpy(p + 8, alpha2, 2);
			for (j = 0; j < count; j++) {
				u8 *entry = p + 44 + j * PKG_ENTRY_SIZE;

				/* A country package has no channel 14 entry. */
				if (!world && pkg_channels[channel] == 14)
					channel++;
				entry[0] = pkg_channels[channel++];
				memset(entry + 1, 30, PKG_ENTRY_SIZE - 1);
			}
			p += 44 + count * PKG_ENTRY_SIZE;
		} else {
			unsigned int k = i ? i - 9 : 0;

			p[1] = 2;
			put_unaligned_le16(lengths[k], p + 2);
			p[7] = types[k];
			if (i == 10)
				memcpy(p + 8, alpha2, 2);
			p += lengths[k];
		}
	}
	KUNIT_ASSERT_PTR_EQ(test, p, data + *size);
	return data;
}

/* The entry of @channel in a parsed package, or NULL. */
static u8 *pkg_entry(const struct mt7932_policy *policy, unsigned int channel)
{
	unsigned int i, j;

	for (i = 1; i <= 9; i++)
		for (j = 0; j < policy->table[i][4]; j++)
			if (policy->table[i][44 + j * PKG_ENTRY_SIZE] == channel)
				return (u8 *)policy->table[i] + 44 + j * PKG_ENTRY_SIZE;
	return NULL;
}

static void mt7932_policy_valid_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t size;
	u8 *data;

	data = pkg_build(test, "AU", &size);
	KUNIT_EXPECT_EQ(test, size, 10447);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "AU"), 0);
	KUNIT_EXPECT_EQ(test, policy.modes, 3);
	KUNIT_EXPECT_PTR_EQ(test, policy.table[0], data + 16 + 3 * PKG_MODE_SIZE);
	KUNIT_EXPECT_EQ(test, policy.length[12], 265);
	KUNIT_EXPECT_TRUE(test, mt7932_policy_permits(&policy, 36));
	KUNIT_EXPECT_FALSE(test, mt7932_policy_permits(&policy, 14));

	data = pkg_build(test, "XZ", &size);
	KUNIT_EXPECT_EQ(test, size, 10241);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "XZ"), 0);
	KUNIT_EXPECT_EQ(test, policy.modes, 2);
	KUNIT_EXPECT_TRUE(test, mt7932_policy_permits(&policy, 14));
}

/* A package serves only the country it names, in the header and table 10. */
static void mt7932_policy_country_mismatch_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t size;
	u8 *data;

	data = pkg_build(test, "AU", &size);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "XZ"), -EINVAL);
	/* A country package renamed to the world one is not the world one. */
	memcpy(data + 6, "XZ", 2);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "XZ"), -EINVAL);
}

/* Every prefix of a valid package, with a consistent header, is rejected. */
static void mt7932_policy_truncation_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t size, cut;
	u8 *data, *copy;

	data = pkg_build(test, "PL", &size);
	for (cut = 0; cut < size; cut++) {
		/* An exact-size copy lets KASAN catch any read past the end. */
		copy = kmemdup(data, cut, GFP_KERNEL);
		KUNIT_ASSERT_TRUE(test, copy || !cut);
		if (cut >= 16)
			put_unaligned_le32(cut, copy + 12);
		KUNIT_EXPECT_EQ_MSG(test, mt7932_policy_parse(&policy, copy, cut, "PL"),
				    -EINVAL, "length %zu", cut);
		kfree(copy);
	}
}

static void mt7932_policy_header_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t size;
	u8 *data, *longer;

	data = pkg_build(test, "PL", &size);
	/* Trailing bytes after the last table. */
	longer = kunit_kzalloc(test, size + 4, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, longer);
	memcpy(longer, data, size);
	put_unaligned_le32(size + 4, longer + 12);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, longer, size + 4, "PL"), -EINVAL);

	data[4] = 2;
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	data[4] = 1;
	data[8] = 4;
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	data[8] = 3;
	data[9] = 12;
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	data[9] = MT7932_POLICY_TABLES;
	put_unaligned_le32(size - 1, data + 12);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	put_unaligned_le32(size, data + 12);
	/* Mode command lengths are bounded by the 320-byte body. */
	put_unaligned_le32(321, data + 16 + 4);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	put_unaligned_le32(0, data + 16 + 4);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	put_unaligned_le32(17, data + 16 + 4);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), 0);
}

/* Table lengths and entry counts must agree with the table type. */
static void mt7932_policy_table_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t size;
	u8 *data, *table;

	data = pkg_build(test, "PL", &size);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), 0);
	table = (u8 *)policy.table[3];
	/* An entry count that would run past the table. */
	table[4] = 9;
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	table[4] = 8;
	/* A table length that claims the rest of the file. */
	put_unaligned_le16(0xffff, table + 2);
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
	put_unaligned_le16(44 + 8 * PKG_ENTRY_SIZE, table + 2);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), 0);
	table = (u8 *)policy.table[10];
	table[7] = 2;
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), -EINVAL);
}

/* The world package's 2.4 GHz tables must list channels 1..14 in order. */
static void mt7932_policy_world_order_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t size;
	u8 *data;

	data = pkg_build(test, "XZ", &size);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "XZ"), 0);
	pkg_entry(&policy, 9)[0] = 10;
	KUNIT_EXPECT_EQ(test, mt7932_policy_parse(&policy, data, size, "XZ"), -EINVAL);
}

static void mt7932_policy_permits_test(struct kunit *test)
{
	struct mt7932_policy policy;
	size_t size;
	u8 *data, *entry;

	data = pkg_build(test, "PL", &size);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), 0);
	entry = pkg_entry(&policy, 144);
	KUNIT_ASSERT_NOT_NULL(test, entry);
	memset(entry + 1, MT7932_POLICY_NOT_PERMITTED, PKG_ENTRY_SIZE - 1);
	KUNIT_EXPECT_FALSE(test, mt7932_policy_permits(&policy, 144));
	/* One rate with a limit is enough to permit the channel. */
	entry[PKG_ENTRY_SIZE - 1] = 8;
	KUNIT_EXPECT_TRUE(test, mt7932_policy_permits(&policy, 144));
	/* A channel the package does not list is not permitted. */
	KUNIT_EXPECT_FALSE(test, mt7932_policy_permits(&policy, 177));
}

static void mt7932_policy_filter_test(struct kunit *test)
{
	static const u16 channels[] = {1, 6, 13, 36, 144, 149, 165};
	struct mt7932_reg_snapshot *reg;
	struct mt7932_policy policy;
	unsigned int i;
	size_t size;
	u8 *data;

	reg = kunit_kzalloc(test, sizeof(*reg), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, reg);
	data = pkg_build(test, "PL", &size);
	KUNIT_ASSERT_EQ(test, mt7932_policy_parse(&policy, data, size, "PL"), 0);
	memset(pkg_entry(&policy, 144) + 1, MT7932_POLICY_NOT_PERMITTED,
	       PKG_ENTRY_SIZE - 1);
	memset(pkg_entry(&policy, 6) + 1, MT7932_POLICY_NOT_PERMITTED,
	       PKG_ENTRY_SIZE - 1);
	memcpy(reg->domain, "PL", 2);
	for (i = 0; i < ARRAY_SIZE(channels); i++) {
		put_unaligned_le16(channels[i], reg->domain + 12 + i * 8);
		put_unaligned_le32(i, reg->domain + 16 + i * 8);
	}
	reg->domain[8] = 3;
	reg->domain[9] = 4;
	reg->length = 12 + ARRAY_SIZE(channels) * 8;

	mt7932_policy_filter(reg, &policy);
	KUNIT_EXPECT_EQ(test, reg->length, 12 + 5 * 8);
	KUNIT_EXPECT_EQ(test, reg->domain[8], 2);
	KUNIT_EXPECT_EQ(test, reg->domain[9], 3);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(reg->domain + 12 + 8), 13);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(reg->domain + 12 + 3 * 8), 149);
	/* Each entry keeps its own channel flags. */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reg->domain + 16 + 3 * 8), 5);
	/* The vacated tail is cleared. */
	KUNIT_EXPECT_TRUE(test, !memchr_inv(reg->domain + 12 + 5 * 8, 0, 2 * 8));
}

/* Random corruption must never be accepted in a way that reads out of bounds. */
static void mt7932_policy_corruption_test(struct kunit *test)
{
	struct rnd_state rnd;
	struct mt7932_policy policy;
	unsigned int round, flips, channel;
	size_t size;
	u8 *data, *copy;

	prandom_seed_state(&rnd, 7932);
	data = pkg_build(test, "AU", &size);
	copy = kunit_kzalloc(test, size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	for (round = 0; round < 2000; round++) {
		memcpy(copy, data, size);
		for (flips = 1 + prandom_u32_state(&rnd) % 4; flips; flips--)
			copy[prandom_u32_state(&rnd) % size] ^= 1 << (prandom_u32_state(&rnd) % 8);
		if (mt7932_policy_parse(&policy, copy, size, "AU"))
			continue;
		for (channel = 0; channel < 256; channel++)
			mt7932_policy_permits(&policy, channel);
	}
}

static struct kunit_case mt7932_policy_test_cases[] = {
	KUNIT_CASE(mt7932_reg_relaxation_test),
	KUNIT_CASE(mt7932_policy_valid_test),
	KUNIT_CASE(mt7932_policy_country_mismatch_test),
	KUNIT_CASE(mt7932_policy_truncation_test),
	KUNIT_CASE(mt7932_policy_header_test),
	KUNIT_CASE(mt7932_policy_table_test),
	KUNIT_CASE(mt7932_policy_world_order_test),
	KUNIT_CASE(mt7932_policy_permits_test),
	KUNIT_CASE(mt7932_policy_filter_test),
	KUNIT_CASE(mt7932_policy_corruption_test),
	{}
};

static struct kunit_suite mt7932_policy_test_suite = {
	.name = "mt7932-regulatory",
	.test_cases = mt7932_policy_test_cases,
};

kunit_test_suite(mt7932_policy_test_suite);
