// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the validation of firmware input: RX descriptors and
 * command replies. The firmware and the device are treated as untrusted.
 */
#include <kunit/test.h>

#include "mt7932.h"

#define RX_HEADER 24
#define RX_ETHERNET 14
#define RX_PAYLOAD 40
#define RX_SIZE (RX_HEADER + RX_ETHERNET + RX_PAYLOAD)

/* A translated (Ethernet) unicast data frame on channel 36 for WTBL 5. */
static void rx_build(u8 *packet, u32 extra_w1)
{
	memset(packet, 0, RX_SIZE);
	put_unaligned_le32(RX_SIZE | (2U << 27), packet);
	put_unaligned_le32(5 | extra_w1, packet + 4);
	put_unaligned_le32(BIT(13), packet + 8);
	put_unaligned_le32((36U << 8) | (1U << 16), packet + 12);
	memset(packet + RX_HEADER, 0xa5, RX_ETHERNET + RX_PAYLOAD);
	put_unaligned_be16(ETH_P_IP, packet + RX_HEADER + 12);
}

static u32 rx_cipher(unsigned int mode)
{
	return mode << MT7932_RXD1_SEC_MODE_SHIFT;
}

static void mt7932_rx_decrypted_test(struct kunit *test)
{
	struct mt7932_rx_frame frame;
	u8 packet[RX_SIZE];

	rx_build(packet, rx_cipher(4));
	KUNIT_ASSERT_EQ(test, mt7932_rx_frame(packet, sizeof(packet), &frame), 0);
	KUNIT_EXPECT_TRUE(test, frame.translated);
	KUNIT_EXPECT_EQ(test, frame.sec_mode, 4);
	KUNIT_EXPECT_EQ(test, frame.wtbl, 5);
	KUNIT_EXPECT_EQ(test, frame.length, (size_t)(RX_ETHERNET + RX_PAYLOAD));
	KUNIT_EXPECT_TRUE(test, mt7932_rx_protection_ok(&frame, false));
	KUNIT_EXPECT_TRUE(test, mt7932_rx_protection_ok(&frame, true));
}

/* A plaintext frame on a protected connection is never delivered. */
static void mt7932_rx_plaintext_test(struct kunit *test)
{
	struct mt7932_rx_frame frame;
	u8 packet[RX_SIZE];

	rx_build(packet, 0);
	KUNIT_ASSERT_EQ(test, mt7932_rx_frame(packet, sizeof(packet), &frame), 0);
	KUNIT_EXPECT_EQ(test, frame.sec_mode, 0);
	KUNIT_EXPECT_FALSE(test, mt7932_rx_protection_ok(&frame, false));
	/* An open connection has nothing to decrypt. */
	KUNIT_EXPECT_TRUE(test, mt7932_rx_protection_ok(&frame, true));
}

/* Decrypted with the wrong cipher or key length: refused on WPA2. */
static void mt7932_rx_mismatch_test(struct kunit *test)
{
	struct mt7932_rx_frame frame;
	u8 packet[RX_SIZE];

	rx_build(packet, rx_cipher(4) | MT7932_RXD1_CIPHER_LEN_MISMATCH);
	KUNIT_ASSERT_EQ(test, mt7932_rx_frame(packet, sizeof(packet), &frame), 0);
	KUNIT_EXPECT_TRUE(test, frame.cipher_mismatch);
	KUNIT_EXPECT_FALSE(test, mt7932_rx_protection_ok(&frame, false));
	/* A translated frame with a cipher mismatch is rejected by the parser. */
	rx_build(packet, rx_cipher(4) | MT7932_RXD1_CIPHER_MISMATCH);
	KUNIT_EXPECT_EQ(test, mt7932_rx_frame(packet, sizeof(packet), &frame), -EBADMSG);
}

/* ICV, TKIP MIC and FCS errors are rejected before any field is used. */
static void mt7932_rx_integrity_test(struct kunit *test)
{
	struct mt7932_rx_frame frame;
	u8 packet[RX_SIZE];
	unsigned int bit;

	for (bit = 25; bit <= 27; bit++) {
		rx_build(packet, rx_cipher(4) | BIT(bit));
		KUNIT_EXPECT_EQ(test, mt7932_rx_frame(packet, sizeof(packet), &frame),
				-EBADMSG);
	}
}

/* Short buffers and length fields beyond the buffer never parse. */
static void mt7932_rx_short_test(struct kunit *test)
{
	struct mt7932_rx_frame frame;
	u8 packet[RX_SIZE], *copy;
	size_t cut;

	rx_build(packet, rx_cipher(4));
	for (cut = 0; cut < RX_HEADER + RX_ETHERNET; cut++) {
		/* An exact-size copy lets KASAN catch any read past the end. */
		copy = kmemdup(packet, cut, GFP_KERNEL);
		KUNIT_ASSERT_TRUE(test, copy || !cut);
		if (cut >= 4)
			put_unaligned_le32(cut | (2U << 27), copy);
		KUNIT_EXPECT_LT_MSG(test, mt7932_rx_frame(copy, cut, &frame), 0,
				    "length %zu", cut);
		kfree(copy);
	}
	/* The descriptor claims more than the DMA buffer holds. */
	copy = kmemdup(packet, RX_SIZE - 1, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	KUNIT_EXPECT_EQ(test, mt7932_rx_frame(copy, RX_SIZE - 1, &frame), -EMSGSIZE);
	kfree(copy);
	/* Optional groups (3 and 5) that do not fit. */
	rx_build(packet, rx_cipher(4) | BIT(13) | BIT(15));
	put_unaligned_le32((RX_HEADER + 20) | (2U << 27), packet);
	KUNIT_EXPECT_EQ(test, mt7932_rx_frame(packet, RX_HEADER + 20, &frame), -EMSGSIZE);
}

static void event_build(u8 *packet, u8 eid, u8 seq, u8 option)
{
	memset(packet, 0, 64);
	put_unaligned_le32(64 | (7U << 27), packet);
	put_unaligned_le16(40, packet + 24);
	packet[28] = eid;
	packet[29] = seq;
	packet[30] = option;
}

/* Only the event that answers a command completes it. */
static void mt7932_event_reply_test(struct kunit *test)
{
	struct mt7932_event event;
	u8 packet[64];
	int eid = mt7932_reply_eid(0xed, true);

	event_build(packet, 0xed, 9, 0);
	KUNIT_ASSERT_EQ(test, mt7932_event_parse(packet, sizeof(packet), &event), 0);
	KUNIT_EXPECT_TRUE(test, mt7932_event_matches(&event, 9, eid));
	/* Another sequence, the no-reply option bit, or nothing waiting. */
	KUNIT_EXPECT_FALSE(test, mt7932_event_matches(&event, 10, eid));
	event.option = 4;
	KUNIT_EXPECT_FALSE(test, mt7932_event_matches(&event, 9, eid));
	event.option = 0;
	KUNIT_EXPECT_FALSE(test, mt7932_event_matches(&event, 0, eid));
	/* An unrelated event that happens to carry the waiting sequence. */
	event_build(packet, 0x99, 9, 0);
	KUNIT_ASSERT_EQ(test, mt7932_event_parse(packet, sizeof(packet), &event), 0);
	KUNIT_EXPECT_FALSE(test, mt7932_event_matches(&event, 9, eid));
	KUNIT_EXPECT_FALSE(test, mt7932_event_matches(&event, 9, mt7932_reply_eid(0x8a, true)));
	KUNIT_EXPECT_FALSE(test, mt7932_event_matches(&event, 9, mt7932_reply_eid(0x03, false)));
	/* The ROM's patch semaphore answers with its own event only. */
	KUNIT_EXPECT_FALSE(test, mt7932_event_matches(&event, 9, mt7932_reply_eid(0x10, false)));
	event_build(packet, 0x04, 9, 0);
	KUNIT_ASSERT_EQ(test, mt7932_event_parse(packet, sizeof(packet), &event), 0);
	KUNIT_EXPECT_TRUE(test, mt7932_event_matches(&event, 9, mt7932_reply_eid(0x10, false)));
	event_build(packet, 0x01, 9, 0);
	KUNIT_ASSERT_EQ(test, mt7932_event_parse(packet, sizeof(packet), &event), 0);
	KUNIT_EXPECT_FALSE(test, mt7932_event_matches(&event, 9, mt7932_reply_eid(0x10, false)));
}

static void mt7932_event_reply_eid_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, mt7932_reply_eid(0xed, false), 0xed);
	KUNIT_EXPECT_EQ(test, mt7932_reply_eid(0xed, true), 0xed);
	KUNIT_EXPECT_EQ(test, mt7932_reply_eid(0x8a, true), 0xec);
	KUNIT_EXPECT_EQ(test, mt7932_reply_eid(0xbf, true), 0xbf);
	KUNIT_EXPECT_EQ(test, mt7932_reply_eid(0x03, false), 0x02);
	KUNIT_EXPECT_EQ(test, mt7932_reply_eid(0x01, false), 0x01);
	KUNIT_EXPECT_EQ(test, mt7932_reply_eid(0x05, false), 0x01);
	KUNIT_EXPECT_EQ(test, mt7932_reply_eid(0x10, false), 0x04);
	KUNIT_EXPECT_EQ(test, mt7932_reply_eid(0x50, false), -1);
}

static struct kunit_case mt7932_input_test_cases[] = {
	KUNIT_CASE(mt7932_rx_decrypted_test),
	KUNIT_CASE(mt7932_rx_plaintext_test),
	KUNIT_CASE(mt7932_rx_mismatch_test),
	KUNIT_CASE(mt7932_rx_integrity_test),
	KUNIT_CASE(mt7932_rx_short_test),
	KUNIT_CASE(mt7932_event_reply_test),
	KUNIT_CASE(mt7932_event_reply_eid_test),
	{}
};

static struct kunit_suite mt7932_input_test_suite = {
	.name = "mt7932-input",
	.test_cases = mt7932_input_test_cases,
};

kunit_test_suite(mt7932_input_test_suite);
