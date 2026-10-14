// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Arm Ltd.
/* This file is intended to be included into mpam_resctrl.c */

#include <kunit/test.h>
#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/math.h>
#include <linux/sprintf.h>

struct percent_value_case {
	u8	pc;
	u8	width;
	u16	value;
};

/*
 * Mysterious inscriptions taken from the union of ARM DDI 0598D.b,
 * "Arm Architecture Reference Manual Supplement - Memory System
 * Resource Partitioning and Monitoring (MPAM), for A-profile
 * architecture", Section 9.8, "About the fixed-point fractional
 * format" (exact percentage entries only) and ARM IHI0099B.a
 * "MPAM system component specification", Section 9.3,
 * "The fixed-point fractional format":
 */
static const struct percent_value_case percent_value_cases[] = {
	/* Architectural cases: */
	{   1,  8,    1 },	{   1, 12,  0x27 },	{   1, 16,  0x28e },
	{  25,  8, 0x3f },	{  25, 12, 0x3ff },	{  25, 16, 0x3fff },
	{  33,  8, 0x53 },	{  33, 12, 0x546 },	{  33, 16, 0x5479 },
	{  35,  8, 0x58 },	{  35, 12, 0x598 },	{  35, 16, 0x5998 },
	{  45,  8, 0x72 },	{  45, 12, 0x732 },	{  45, 16, 0x7332 },
	{  50,  8, 0x7f },	{  50, 12, 0x7ff },	{  50, 16, 0x7fff },
	{  52,  8, 0x84 },	{  52, 12, 0x850 },	{  52, 16, 0x851d },
	{  55,  8, 0x8b },	{  55, 12, 0x8cb },	{  55, 16, 0x8ccb },
	{  58,  8, 0x93 },	{  58, 12, 0x946 },	{  58, 16, 0x9479 },
	{  75,  8, 0xbf },	{  75, 12, 0xbff },	{  75, 16, 0xbfff },
	{  80,  8, 0xcb },	{  80, 12, 0xccb },	{  80, 16, 0xcccb },
	{  88,  8, 0xe0 },	{  88, 12, 0xe13 },	{  88, 16, 0xe146 },
	{  95,  8, 0xf2 },	{  95, 12, 0xf32 },	{  95, 16, 0xf332 },
	{ 100,  8, 0xff },	{ 100, 12, 0xfff },	{ 100, 16, 0xffff },
};

static void test_percent_value_desc(const struct percent_value_case *param,
				    char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE,
		 "pc=%d, width=%d, value=0x%.*x\n",
		 param->pc, param->width,
		 DIV_ROUND_UP(param->width, 4), param->value);
}

KUNIT_ARRAY_PARAM(test_percent_value, percent_value_cases,
		  test_percent_value_desc);

struct percent_value_test_info {
	u32 pc;			/* result of value-to-percent conversion */
	u32 value;		/* result of percent-to-value conversion */
	u32 max_value;		/* maximum raw value allowed by test params */
	unsigned int shift;	/* promotes raw testcase value to 16 bits */
};

/*
 * Convert a reference percentage to a fixed-point MAX value and
 * vice-versa, based on param (not test->param_value!)
 */
static void __prepare_percent_value_test(struct kunit *test,
					 struct percent_value_test_info *res,
					 const struct percent_value_case *param)
{
	struct mpam_props fake_props = { };

	/* Reject bogus test parameters that would break the tests: */
	KUNIT_ASSERT_GE(test, param->width, 1);
	KUNIT_ASSERT_LE(test, param->width, 16);
	KUNIT_ASSERT_LT(test, param->value, 1 << param->width);

	mpam_set_feature(mpam_feat_mbw_max, &fake_props);
	fake_props.bwa_wd = param->width;

	res->shift = 16 - param->width;
	res->max_value = GENMASK_U32(param->width - 1, 0);
	res->value = percent_to_mbw_max(param->pc, &fake_props);
	res->pc = mbw_max_to_percent(param->value << res->shift, &fake_props);
}

static void test_get_mba_granularity(struct kunit *test)
{
	int ret;
	struct mpam_props fake_props = { };

	/* Use MBW_MAX */
	mpam_set_feature(mpam_feat_mbw_max, &fake_props);

	fake_props.bwa_wd = 0;
	KUNIT_EXPECT_FALSE(test, mba_class_use_mbw_max(&fake_props));

	fake_props.bwa_wd = 1;
	KUNIT_EXPECT_TRUE(test, mba_class_use_mbw_max(&fake_props));

	/* Architectural maximum: */
	fake_props.bwa_wd = 16;
	KUNIT_EXPECT_TRUE(test, mba_class_use_mbw_max(&fake_props));

	/* No usable control... */
	fake_props.bwa_wd = 0;
	ret = get_mba_granularity(&fake_props);
	KUNIT_EXPECT_EQ(test, ret, 0);

	fake_props.bwa_wd = 1;
	ret = get_mba_granularity(&fake_props);
	KUNIT_EXPECT_EQ(test, ret, 50);	/* DIV_ROUND_UP(100, 1 << 1)% = 50% */

	fake_props.bwa_wd = 2;
	ret = get_mba_granularity(&fake_props);
	KUNIT_EXPECT_EQ(test, ret, 25);	/* DIV_ROUND_UP(100, 1 << 2)% = 25% */

	fake_props.bwa_wd = 3;
	ret = get_mba_granularity(&fake_props);
	KUNIT_EXPECT_EQ(test, ret, 13);	/* DIV_ROUND_UP(100, 1 << 3)% = 13% */

	fake_props.bwa_wd = 6;
	ret = get_mba_granularity(&fake_props);
	KUNIT_EXPECT_EQ(test, ret, 2);	/* DIV_ROUND_UP(100, 1 << 6)% = 2% */

	fake_props.bwa_wd = 7;
	ret = get_mba_granularity(&fake_props);
	KUNIT_EXPECT_EQ(test, ret, 1);	/* DIV_ROUND_UP(100, 1 << 7)% = 1% */

	/* Granularity saturates at 1% */
	fake_props.bwa_wd = 16; /* architectural maximum */
	ret = get_mba_granularity(&fake_props);
	KUNIT_EXPECT_EQ(test, ret, 1);	/* DIV_ROUND_UP(100, 1 << 16)% = 1% */
}

static void test_mbw_max_to_percent(struct kunit *test)
{
	const struct percent_value_case *param = test->param_value;
	struct percent_value_test_info res;

	/*
	 * Since the reference values in percent_value_cases[] all
	 * correspond to exact percentages, round-to-nearest will
	 * always give the exact percentage back when the MPAM max
	 * value has precision of 0.5% or finer.  (Always true for the
	 * reference data, since they all specify 8 bits or more of
	 * precision.
	 *
	 * So, keep it simple and demand an exact match:
	 */
	__prepare_percent_value_test(test, &res, param);
	KUNIT_EXPECT_EQ(test, res.pc, param->pc);
}

static void test_percent_to_mbw_max(struct kunit *test)
{
	const struct percent_value_case *param = test->param_value;
	struct percent_value_test_info res;

	__prepare_percent_value_test(test, &res, param);

	KUNIT_EXPECT_GE(test, res.value, param->value << res.shift);
	KUNIT_EXPECT_LE(test, res.value, (param->value + 1) << res.shift);
	KUNIT_EXPECT_LE(test, res.value, res.max_value << res.shift);

	/* No flexibility allowed for 0% and 100%! */

	if (param->pc == 0)
		KUNIT_EXPECT_EQ(test, res.value, 0);

	if (param->pc == 100)
		KUNIT_EXPECT_EQ(test, res.value, res.max_value << res.shift);
}

static const void *test_all_bwa_wd_gen_params(struct kunit *test, const void *prev,
					      char *desc)
{
	uintptr_t param = (uintptr_t)prev;

	if (param > 15)
		return NULL;

	param++;

	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "wd=%u\n", (unsigned int)param);

	return (void *)param;
}

static unsigned int test_get_bwa_wd(struct kunit *test)
{
	uintptr_t param = (uintptr_t)test->param_value;

	KUNIT_ASSERT_GE(test, param, 1);
	KUNIT_ASSERT_LE(test, param, 16);

	return param;
}

static void test_mbw_max_to_percent_limits(struct kunit *test)
{
	struct mpam_props fake_props = {0};
	u32 max_value;

	mpam_set_feature(mpam_feat_mbw_max, &fake_props);
	fake_props.bwa_wd = test_get_bwa_wd(test);
	max_value = GENMASK(15, 16 - fake_props.bwa_wd);

	KUNIT_EXPECT_EQ(test, mbw_max_to_percent(max_value, &fake_props),
			MAX_MBA_BW);
	KUNIT_EXPECT_EQ(test, mbw_max_to_percent(0, &fake_props),
			get_mba_min(&fake_props));

	/*
	 * Rounding policy dependent 0% sanity-check:
	 * With round-to-nearest, the minimum mbw_max value really
	 * should map to 0% if there are at least 200 steps.
	 * (100 steps may be enough for some other rounding policies.)
	 */
	if (fake_props.bwa_wd >= 8)
		KUNIT_EXPECT_EQ(test, mbw_max_to_percent(0, &fake_props), 0);

	if (fake_props.bwa_wd < 8 &&
	    mbw_max_to_percent(0, &fake_props) == 0)
		kunit_warn(test, "wd=%d: Testsuite/driver Rounding policy mismatch?",
			   fake_props.bwa_wd);
}

/*
 * Check that converting a percentage to mbw_max and back again (or, as
 * appropriate, vice-versa) always restores the original value:
 */
static void test_percent_max_roundtrip_stability(struct kunit *test)
{
	struct mpam_props fake_props = {0};
	unsigned int shift;
	u32 pc, max, pc2, max2;

	mpam_set_feature(mpam_feat_mbw_max, &fake_props);
	fake_props.bwa_wd = test_get_bwa_wd(test);
	shift = 16 - fake_props.bwa_wd;

	/*
	 * Converting a valid value from the coarser scale to the finer
	 * scale and back again must yield the original value:
	 */
	if (fake_props.bwa_wd >= 7) {
		/* More than 100 steps: only test exact pc values: */
		for (pc = get_mba_min(&fake_props); pc <= MAX_MBA_BW; pc++) {
			max = percent_to_mbw_max(pc, &fake_props);
			pc2 = mbw_max_to_percent(max, &fake_props);
			KUNIT_EXPECT_EQ(test, pc2, pc);
		}
	} else {
		/* Fewer than 100 steps: only test exact mbw_max values: */
		for (max = 0; max < 1 << 16; max += 1 << shift) {
			pc = mbw_max_to_percent(max, &fake_props);
			max2 = percent_to_mbw_max(pc, &fake_props);
			KUNIT_EXPECT_EQ(test, max2, max);
		}
	}
}

static void test_percent_to_max_rounding(struct kunit *test)
{
	const struct percent_value_case *param = test->param_value;
	unsigned int num_rounded_up = 0, total = 0;
	struct percent_value_test_info res;

	for (param = percent_value_cases, total = 0;
	     param < &percent_value_cases[ARRAY_SIZE(percent_value_cases)];
	     param++, total++) {
		__prepare_percent_value_test(test, &res, param);
		if (res.value > param->value << res.shift)
			num_rounded_up++;
	}

	/*
	 * The MPAM driver applies a round-to-nearest policy, whereas a
	 * round-down policy seems to have been applied in the
	 * reference table from which the test vectors were selected.
	 *
	 * For a large and well-distributed suite of test vectors,
	 * about half should be rounded up and half down compared with
	 * the reference table.  The actual test vectors are few in
	 * number and probably not very well distributed however, so
	 * tolerate a round-up rate of between 1/4 and 3/4 before
	 * crying foul:
	 */

	kunit_info(test, "Round-up rate: %u%% (%u/%u)\n",
		   DIV_ROUND_CLOSEST(num_rounded_up * 100, total),
		   num_rounded_up, total);

	KUNIT_EXPECT_GE(test, 4 * num_rounded_up, 1 * total);
	KUNIT_EXPECT_LE(test, 4 * num_rounded_up, 3 * total);
}

/*
 * Test cases for CDP and PARTID narrowing compatibility
 */

struct cdp_narrowing_test_case {
	bool cdp_enabled;
	bool narrowing_enabled;
	u32 closid;
	u32 rmid;
	u8 pmg_max;
	u32 expected_partid_d;
	u32 expected_partid_i;
	u8 expected_pmg_d;
	u8 expected_pmg_i;
	const char *desc;
};

static const struct cdp_narrowing_test_case cdp_narrowing_cases[] = {
	{
		.cdp_enabled = false,
		.narrowing_enabled = false,
		.closid = 5,
		.rmid = 3,
		.pmg_max = 127,
		.expected_partid_d = 5,
		.expected_partid_i = 5,
		.expected_pmg_d = 3,
		.expected_pmg_i = 3,
		.desc = "CDP=0, narrow=0: basic mapping"
	},
	{
		.cdp_enabled = false,
		.narrowing_enabled = true,
		.closid = 5,
		.rmid = 258,
		.pmg_max = 127,
		.expected_partid_d = 2,
		.expected_partid_i = 2,
		.expected_pmg_d = 2,
		.expected_pmg_i = 2,
		.desc = "CDP=0, narrow=1: extract reqPARTID from rmid"
	},
	{
		.cdp_enabled = true,
		.narrowing_enabled = false,
		.closid = 5,
		.rmid = 3,
		.pmg_max = 127,
		.expected_partid_d = 10,
		.expected_partid_i = 11,
		.expected_pmg_d = 3,
		.expected_pmg_i = 3,
		.desc = "CDP=1, narrow=0: CDP DATA/CODE split"
	},
	{
		.cdp_enabled = true,
		.narrowing_enabled = true,
		.closid = 5,
		.rmid = 258,
		.pmg_max = 127,
		.expected_partid_d = 4,
		.expected_partid_i = 5,
		.expected_pmg_d = 2,
		.expected_pmg_i = 2,
		.desc = "CDP=1, narrow=1: both features combined"
	},
	{
		.cdp_enabled = false,
		.narrowing_enabled = false,
		.closid = 65535,
		.rmid = 0,
		.pmg_max = 127,
		.expected_partid_d = 65535,
		.expected_partid_i = 65535,
		.expected_pmg_d = 0,
		.expected_pmg_i = 0,
		.desc = "Boundary: max closid"
	},
	{
		.cdp_enabled = false,
		.narrowing_enabled = false,
		.closid = 0,
		.rmid = 127,
		.pmg_max = 127,
		.expected_partid_d = 0,
		.expected_partid_i = 0,
		.expected_pmg_d = 127,
		.expected_pmg_i = 127,
		.desc = "Boundary: max pmg"
	},
	{
		.cdp_enabled = false,
		.narrowing_enabled = true,
		.closid = 0,
		.rmid = 639,
		.pmg_max = 159,
		.expected_partid_d = 3,
		.expected_partid_i = 3,
		.expected_pmg_d = 159,
		.expected_pmg_i = 159,
		.desc = "Narrowing with pmg_max=159"
	},
	{
		.cdp_enabled = true,
		.narrowing_enabled = true,
		.closid = 10,
		.rmid = 1280,
		.pmg_max = 127,
		.expected_partid_d = 20,
		.expected_partid_i = 21,
		.expected_pmg_d = 0,
		.expected_pmg_i = 0,
		.desc = "CDP + narrowing with reqPARTID=10"
	},
};

static void cdp_narrowing_test_desc(const struct cdp_narrowing_test_case *param,
				    char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%s", param->desc);
}

KUNIT_ARRAY_PARAM(cdp_narrowing, cdp_narrowing_cases, cdp_narrowing_test_desc);

static void test_cdp_narrowing_mapping(struct kunit *test)
{
	const struct cdp_narrowing_test_case *param = test->param_value;
	bool saved_cdp, saved_narrowing;
	u32 partid_d, partid_i;
	u8 saved_pmg_max;
	u8 pmg_d, pmg_i;

	saved_pmg_max = mpam_pmg_max;
	saved_cdp = cdp_enabled;
	saved_narrowing = resctrl_using_extra_partids;

	mpam_pmg_max = param->pmg_max;
	cdp_enabled = param->cdp_enabled;
	resctrl_using_extra_partids = param->narrowing_enabled;

	resctrl_closid_rmid_to_partid_pmg(param->closid, param->rmid,
					  &partid_d, &partid_i,
					  &pmg_d, &pmg_i);

	KUNIT_EXPECT_EQ(test, partid_d, param->expected_partid_d);
	KUNIT_EXPECT_EQ(test, partid_i, param->expected_partid_i);
	KUNIT_EXPECT_EQ(test, pmg_d, param->expected_pmg_d);
	KUNIT_EXPECT_EQ(test, pmg_i, param->expected_pmg_i);

	mpam_pmg_max = saved_pmg_max;
	cdp_enabled = saved_cdp;
	resctrl_using_extra_partids = saved_narrowing;
}

static void test_cdp_narrowing_consistency(struct kunit *test)
{
	u32 partid_d1, partid_i1, partid_d2, partid_i2;
	u8 pmg_d1, pmg_i1, pmg_d2, pmg_i2;
	bool saved_cdp, saved_narrowing;
	u8 saved_pmg_max;

	saved_pmg_max = mpam_pmg_max;
	saved_cdp = cdp_enabled;
	saved_narrowing = resctrl_using_extra_partids;

	mpam_pmg_max = 127;

	cdp_enabled = true;
	resctrl_using_extra_partids = false;
	resctrl_closid_rmid_to_partid_pmg(10, 5, &partid_d1, &partid_i1,
					  &pmg_d1, &pmg_i1);

	cdp_enabled = true;
	resctrl_using_extra_partids = false;
	resctrl_closid_rmid_to_partid_pmg(10, 7, &partid_d2, &partid_i2,
					  &pmg_d2, &pmg_i2);

	KUNIT_EXPECT_EQ(test, partid_d1, partid_d2);
	KUNIT_EXPECT_EQ(test, partid_i1, partid_i2);
	KUNIT_EXPECT_EQ(test, partid_i1, partid_d1 + 1);

	mpam_pmg_max = saved_pmg_max;
	cdp_enabled = saved_cdp;
	resctrl_using_extra_partids = saved_narrowing;
}

static void test_narrowing_rmid_encode_decode(struct kunit *test)
{
	bool saved_narrowing;
	u8 pmg, decoded_pmg;
	u8 saved_pmg_max;
	u16 partid;
	u32 rmid;

	saved_pmg_max = mpam_pmg_max;
	saved_narrowing = resctrl_using_extra_partids;

	mpam_pmg_max = 127;
	resctrl_using_extra_partids = true;

	for (partid = 0; partid < 100; partid++) {
		for (pmg = 0; pmg <= mpam_pmg_max; pmg += 10) {
			rmid = partid * (mpam_pmg_max + 1) + pmg;

			KUNIT_EXPECT_EQ(test, resctrl_rmid_to_partid(rmid), partid);
			decoded_pmg = resctrl_rmid_to_pmg(rmid);
			KUNIT_EXPECT_EQ(test, decoded_pmg, pmg);
		}
	}

	mpam_pmg_max = saved_pmg_max;
	resctrl_using_extra_partids = saved_narrowing;
}

static void test_narrowing_rmid_idx_encode(struct kunit *test)
{
	u32 closid, rmid, idx;
	bool saved_narrowing;
	u8 saved_pmg_max;

	saved_pmg_max = mpam_pmg_max;
	saved_narrowing = resctrl_using_extra_partids;

	mpam_pmg_max = 127;

	resctrl_using_extra_partids = false;
	for (closid = 0; closid < 10; closid++) {
		for (rmid = 0; rmid < 10; rmid++) {
			idx = resctrl_arch_rmid_idx_encode(closid, rmid);
			KUNIT_EXPECT_EQ(test, idx, closid * 128 + rmid);
		}
	}

	resctrl_using_extra_partids = true;
	for (closid = 0; closid < 10; closid++) {
		for (rmid = 0; rmid < 1000; rmid += 100) {
			idx = resctrl_arch_rmid_idx_encode(closid, rmid);
			KUNIT_EXPECT_EQ(test, idx, rmid);
		}
	}

	mpam_pmg_max = saved_pmg_max;
	resctrl_using_extra_partids = saved_narrowing;
}

static void test_narrowing_rmid_idx_decode(struct kunit *test)
{
	u32 decoded_closid, decoded_rmid;
	u32 idx, closid, rmid;
	bool saved_narrowing;
	u8 saved_pmg_max;

	saved_pmg_max = mpam_pmg_max;
	saved_narrowing = resctrl_using_extra_partids;

	mpam_pmg_max = 127;

	resctrl_using_extra_partids = false;
	for (closid = 0; closid < 10; closid++) {
		for (rmid = 0; rmid < 10; rmid++) {
			idx = closid * 128 + rmid;
			resctrl_arch_rmid_idx_decode(idx, &decoded_closid, &decoded_rmid);
			KUNIT_EXPECT_EQ(test, decoded_closid, closid);
			KUNIT_EXPECT_EQ(test, decoded_rmid, rmid);
		}
	}

	resctrl_using_extra_partids = true;
	mpam_partid_map_init(64, 32);
	for (idx = 0; idx < 5000; idx += 500) {
		u16 partid = resctrl_rmid_to_partid(idx);
		u16 intpartid = mpam_partid_to_intpartid(partid);

		resctrl_arch_rmid_idx_decode(idx, &decoded_closid, &decoded_rmid);
		KUNIT_EXPECT_EQ(test, decoded_closid, intpartid);
		KUNIT_EXPECT_EQ(test, decoded_rmid, idx);
	}
	mpam_partid_map_init(0, 0);

	mpam_pmg_max = saved_pmg_max;
	resctrl_using_extra_partids = saved_narrowing;
}

struct partid_mapping_case {
	u16 partid;
	u16 num_partid;
	u16 num_closid;
	u16 expected_intpartid;
	const char *desc;
};

static const struct partid_mapping_case partid_mapping_cases[] = {
	{ 0, 128, 32, 0, "First PARTID maps to first intPARTID" },
	{ 31, 128, 32, 31, "Last intPARTID maps 1:1" },
	{ 32, 128, 32, 0, "First extra PARTID wraps to intPARTID 0" },
	{ 33, 128, 32, 1, "Second extra PARTID wraps to intPARTID 1" },
	{ 63, 128, 32, 31, "PARTID 63 wraps to intPARTID 31" },
	{ 64, 128, 32, 0, "PARTID 64 wraps to intPARTID 0" },
	{ 127, 128, 32, 31, "Max PARTID wraps to intPARTID 31" },
	{ 15, 64, 16, 15, "Small config: PARTID 15 → intPARTID 15" },
	{ 16, 64, 16, 0, "Small config: PARTID 16 wraps to 0" },
	{ 63, 64, 16, 15, "Small config: max PARTID wraps to 15" },
};

static void partid_mapping_desc(const struct partid_mapping_case *param,
				char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%s", param->desc);
}

KUNIT_ARRAY_PARAM(partid_mapping, partid_mapping_cases, partid_mapping_desc);

static void test_narrowing_partid_mapping(struct kunit *test)
{
	const struct partid_mapping_case *param = test->param_value;
	u16 result;

	mpam_partid_map_init(param->num_partid, param->num_closid);

	result = mpam_partid_to_intpartid(param->partid);
	KUNIT_EXPECT_EQ(test, result, param->expected_intpartid);

	mpam_partid_map_init(0, 0);
}

struct partid_in_closid_case {
	u16 partid;
	u32 closid;
	bool cdp_enabled;
	bool narrowing_enabled;
	u16 num_closid;
	bool expected;
	const char *desc;
};

static const struct partid_in_closid_case partid_in_closid_cases[] = {
	{ 10, 10, false, false, 32, true, "Legacy: exact match" },
	{ 10, 11, false, false, 32, false, "Legacy: no match" },
	{ 20, 10, true, false, 32, true, "CDP: even PARTID for closid 10" },
	{ 21, 10, true, false, 32, true, "CDP: odd PARTID for closid 10" },
	{ 22, 10, true, false, 32, false, "CDP: PARTID 22 not in closid 10" },
	{ 10, 10, false, true, 32, true, "Narrowing: PARTID 10 -> intPARTID 10" },
	{ 42, 10, false, true, 32, true, "Narrowing: PARTID 42 wraps to intPARTID 10" },
	{ 74, 10, false, true, 32, true, "Narrowing: PARTID 74 wraps to intPARTID 10" },
	{ 43, 10, false, true, 32, false, "Narrowing: PARTID 43 -> intPARTID 11" },
	{ 20, 10, true, true, 32, true, "CDP+narrow: even PARTID 20 -> intPARTID 10" },
	{ 21, 10, true, true, 32, true, "CDP+narrow: odd PARTID 21 -> intPARTID 10" },
	{ 84, 10, true, true, 32, true, "CDP+narrow: PARTID 84 wraps to intPARTID 10" },
	{ 86, 10, true, true, 32, false, "CDP+narrow: PARTID 86 -> intPARTID 11" },
};

static void partid_in_closid_desc(const struct partid_in_closid_case *param,
				  char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%s", param->desc);
}

KUNIT_ARRAY_PARAM(partid_in_closid, partid_in_closid_cases,
		  partid_in_closid_desc);

static void test_narrowing_partid_in_closid(struct kunit *test)
{
	const struct partid_in_closid_case *param = test->param_value;
	bool saved_cdp, saved_narrowing;
	bool result;

	saved_cdp = cdp_enabled;
	saved_narrowing = resctrl_using_extra_partids;

	cdp_enabled = param->cdp_enabled;
	resctrl_using_extra_partids = param->narrowing_enabled;
	mpam_partid_map_init(128, param->num_closid);

	result = resctrl_partid_in_closid(param->partid, param->closid);
	KUNIT_EXPECT_EQ(test, result, param->expected);

	mpam_partid_map_init(0, 0);
	cdp_enabled = saved_cdp;
	resctrl_using_extra_partids = saved_narrowing;
}

static void test_narrowing_boundary_conditions(struct kunit *test)
{
	bool saved_narrowing;
	u8 saved_pmg_max;
	u32 rmid, idx;
	u16 partid;
	u8 pmg;

	saved_pmg_max = mpam_pmg_max;
	saved_narrowing = resctrl_using_extra_partids;

	resctrl_using_extra_partids = true;

	mpam_pmg_max = 255;
	rmid = 255 * 256 + 255;
	partid = resctrl_rmid_to_partid(rmid);
	pmg = resctrl_rmid_to_pmg(rmid);
	KUNIT_EXPECT_EQ(test, partid, 255);
	KUNIT_EXPECT_EQ(test, pmg, 255);

	mpam_pmg_max = 127;
	rmid = 100 * 128 + 127;
	partid = resctrl_rmid_to_partid(rmid);
	pmg = resctrl_rmid_to_pmg(rmid);
	KUNIT_EXPECT_EQ(test, partid, 100);
	KUNIT_EXPECT_EQ(test, pmg, 127);

	mpam_pmg_max = 0;
	rmid = 50;
	partid = resctrl_rmid_to_partid(rmid);
	pmg = resctrl_rmid_to_pmg(rmid);
	KUNIT_EXPECT_EQ(test, partid, 50);
	KUNIT_EXPECT_EQ(test, pmg, 0);

	mpam_pmg_max = 15;
	idx = resctrl_arch_rmid_idx_encode(0, 1000);
	KUNIT_EXPECT_EQ(test, idx, 1000);

	mpam_pmg_max = saved_pmg_max;
	resctrl_using_extra_partids = saved_narrowing;
}

static void test_narrowing_full_range_mapping(struct kunit *test)
{
	u16 partid, intpartid;
	bool saved_narrowing;
	u8 saved_pmg_max;
	int count = 0;
	u32 rmid;

	saved_pmg_max = mpam_pmg_max;
	saved_narrowing = resctrl_using_extra_partids;

	mpam_pmg_max = 31;
	resctrl_using_extra_partids = true;
	mpam_partid_map_init(64, 16);

	for (rmid = 0; rmid < 2048; rmid += 32) {
		partid = resctrl_rmid_to_partid(rmid);
		intpartid = mpam_partid_to_intpartid(partid);

		KUNIT_EXPECT_LT(test, partid, 64);
		KUNIT_EXPECT_LT(test, intpartid, 16);
		KUNIT_EXPECT_EQ(test, intpartid, partid % 16);

		count++;
	}

	KUNIT_EXPECT_GT(test, count, 0);

	mpam_partid_map_init(0, 0);
	mpam_pmg_max = saved_pmg_max;
	resctrl_using_extra_partids = saved_narrowing;
}

static struct kunit_case mpam_resctrl_test_cases[] = {
	KUNIT_CASE(test_get_mba_granularity),
	KUNIT_CASE_PARAM(test_mbw_max_to_percent, test_percent_value_gen_params),
	KUNIT_CASE_PARAM(test_percent_to_mbw_max, test_percent_value_gen_params),
	KUNIT_CASE_PARAM(test_mbw_max_to_percent_limits, test_all_bwa_wd_gen_params),
	KUNIT_CASE(test_percent_to_max_rounding),
	KUNIT_CASE_PARAM(test_percent_max_roundtrip_stability,
			 test_all_bwa_wd_gen_params),
	KUNIT_CASE_PARAM(test_cdp_narrowing_mapping, cdp_narrowing_gen_params),
	KUNIT_CASE(test_cdp_narrowing_consistency),
	KUNIT_CASE(test_narrowing_rmid_encode_decode),
	KUNIT_CASE(test_narrowing_rmid_idx_encode),
	KUNIT_CASE(test_narrowing_rmid_idx_decode),
	KUNIT_CASE_PARAM(test_narrowing_partid_mapping, partid_mapping_gen_params),
	KUNIT_CASE_PARAM(test_narrowing_partid_in_closid,
			 partid_in_closid_gen_params),
	KUNIT_CASE(test_narrowing_boundary_conditions),
	KUNIT_CASE(test_narrowing_full_range_mapping),
	{}
};

static struct kunit_suite mpam_resctrl_test_suite = {
	.name = "mpam_resctrl_test_suite",
	.test_cases = mpam_resctrl_test_cases,
};

kunit_test_suites(&mpam_resctrl_test_suite);
