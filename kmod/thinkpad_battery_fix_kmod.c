// SPDX-License-Identifier: GPL-2.0
/*
 * thinkpad_battery_fix_kmod.c
 *
 * Kernel power_supply property hook for Lenovo ThinkPad batteries hit by the
 * EC 7%/0W EDV2 latch where hardware SBAC remains 0 uW after low-voltage trip.
 *
 * Works alongside the userspace thinkpad-battery-fix daemon:
 *  - Exposes unmodified ACPI hardware values via read-only module parameters:
 *    raw_power_uw, raw_energy_uwh, raw_status.
 *  - Overrides zeroed POWER_SUPPLY_PROP_POWER_NOW / CURRENT_NOW using live
 *    rate telemetry supplied via override_power_uw (or EC/default fallback).
 *  - Optionally smooths discharging energy/capacity via override_energy_uwh
 *    when the EC freezes SBRC while on battery.
 *  - Masks transient POWER_SUPPLY_STATUS_NOT_CHARGING (pending-charge) during
 *    EC SMBus inhibit-charge -> auto refresh pulses when mask_not_charging != 0.
 *  - Normalizes stuck POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL once capacity recovers.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/acpi.h>
#include <linux/math64.h>
#include <linux/rcupdate.h>
#include <acpi/battery.h>

#define MAX_HOOKED_BATTERIES 4

static int override_power_uw;
module_param(override_power_uw, int, 0644);
MODULE_PARM_DESC(override_power_uw, "Live battery charge/discharge power override in uW (0 = auto fallback)");

static int override_energy_uwh;
module_param(override_energy_uwh, int, 0644);
MODULE_PARM_DESC(override_energy_uwh, "Live battery energy_now/charge_now override in uWh/uAh (0 = passthrough)");

static int mask_not_charging;
module_param(mask_not_charging, int, 0644);
MODULE_PARM_DESC(mask_not_charging, "When non-zero, map NOT_CHARGING to CHARGING during EC SMBus refresh pulse");

static int raw_power_uw;
module_param(raw_power_uw, int, 0444);
MODULE_PARM_DESC(raw_power_uw, "Last raw hardware power_now reported by acpi-battery (uW)");

static int raw_energy_uwh;
module_param(raw_energy_uwh, int, 0444);
MODULE_PARM_DESC(raw_energy_uwh, "Last raw hardware energy_now reported by acpi-battery (uWh)");

static int raw_status;
module_param(raw_status, int, 0444);
MODULE_PARM_DESC(raw_status, "Last raw hardware status enum reported by acpi-battery");

struct hooked_battery {
	struct power_supply *psy;
	struct power_supply_desc *desc;
	int (*orig_get_property)(struct power_supply *psy,
				 enum power_supply_property psp,
				 union power_supply_propval *val);
};

static DEFINE_MUTEX(hook_lock);
static struct hooked_battery hooked_bats[MAX_HOOKED_BATTERIES];

static int (*find_orig_getter(struct power_supply *psy))(struct power_supply *,
							 enum power_supply_property,
							 union power_supply_propval *)
{
	int i;

	for (i = 0; i < MAX_HOOKED_BATTERIES; i++) {
		if (READ_ONCE(hooked_bats[i].psy) == psy)
			return READ_ONCE(hooked_bats[i].orig_get_property);
	}
	return NULL;
}

static u16 ec_read_u16(u8 lo_off, u8 hi_off, bool *ok)
{
	u8 lo = 0, hi = 0;

	if (ec_read(lo_off, &lo) != 0 || ec_read(hi_off, &hi) != 0) {
		*ok = false;
		return 0;
	}
	return (u16)lo | ((u16)hi << 8);
}

static int estimate_ec_fallback_power_uw(int status_val)
{
	u8 hiid = 0;
	bool ok = true;
	u16 sbrc, sbfc, sbae, sbaf;
	s64 rem_mwh, full_mwh, pw_mw;

	if (ec_read(0x81, &hiid) == 0 && (hiid & 0x0f) == 0x00) {
		sbrc = ec_read_u16(0xa0, 0xa1, &ok);
		sbfc = ec_read_u16(0xa2, 0xa3, &ok);
		sbae = ec_read_u16(0xa4, 0xa5, &ok);
		sbaf = ec_read_u16(0xac, 0xad, &ok);

		if (ok) {
			rem_mwh = (s64)sbrc * 10LL;
			full_mwh = (s64)sbfc * 10LL;
			if (status_val == POWER_SUPPLY_STATUS_CHARGING &&
			    sbaf > 0 && sbaf < 600 && full_mwh > rem_mwh) {
				pw_mw = div64_s64((full_mwh - rem_mwh) * 60LL, (s64)sbaf);
				if (pw_mw >= 2000 && pw_mw <= 65000)
					return (int)(pw_mw * 1000LL);
			} else if (status_val == POWER_SUPPLY_STATUS_DISCHARGING &&
				   sbae > 0 && sbae < 1200 && rem_mwh > 0) {
				pw_mw = div64_s64(rem_mwh * 60LL, (s64)sbae);
				if (pw_mw >= 2500 && pw_mw <= 65000)
					return (int)(pw_mw * 1000LL);
			}
		}
	}

	return (status_val == POWER_SUPPLY_STATUS_CHARGING) ? 15000000 : 6500000;
}

static int tbf_get_effective_status(struct power_supply *psy,
				    int (*orig)(struct power_supply *,
						enum power_supply_property,
						union power_supply_propval *))
{
	union power_supply_propval st;
	int ret;

	ret = orig(psy, POWER_SUPPLY_PROP_STATUS, &st);
	if (ret < 0)
		return POWER_SUPPLY_STATUS_UNKNOWN;

	WRITE_ONCE(raw_status, st.intval);
	if (READ_ONCE(mask_not_charging) != 0 &&
	    st.intval == POWER_SUPPLY_STATUS_NOT_CHARGING)
		return POWER_SUPPLY_STATUS_CHARGING;

	return st.intval;
}

static int tbf_get_property(struct power_supply *psy,
			    enum power_supply_property psp,
			    union power_supply_propval *val)
{
	int (*orig)(struct power_supply *, enum power_supply_property,
		    union power_supply_propval *);
	int ret, eff_status, pw, ov_e;

	orig = find_orig_getter(psy);
	if (!orig)
		return -ENODEV;

	ret = orig(psy, psp, val);
	if (ret < 0)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		WRITE_ONCE(raw_status, val->intval);
		if (READ_ONCE(mask_not_charging) != 0 &&
		    val->intval == POWER_SUPPLY_STATUS_NOT_CHARGING)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		break;

	case POWER_SUPPLY_PROP_POWER_NOW:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		if (psp == POWER_SUPPLY_PROP_POWER_NOW)
			WRITE_ONCE(raw_power_uw, val->intval);

		if (val->intval == 0) {
			eff_status = tbf_get_effective_status(psy, orig);
			if (eff_status == POWER_SUPPLY_STATUS_CHARGING ||
			    eff_status == POWER_SUPPLY_STATUS_DISCHARGING) {
				pw = READ_ONCE(override_power_uw);
				if (pw <= 0)
					pw = estimate_ec_fallback_power_uw(eff_status);

				if (psp == POWER_SUPPLY_PROP_POWER_NOW) {
					val->intval = pw;
				} else {
					union power_supply_propval volt;

					if (orig(psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &volt) == 0 &&
					    volt.intval > 1000000)
						val->intval = (int)div64_u64((u64)pw * 1000000ULL,
									     (u64)volt.intval);
				}
			}
		}
		break;

	case POWER_SUPPLY_PROP_ENERGY_NOW:
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		WRITE_ONCE(raw_energy_uwh, val->intval);
		ov_e = READ_ONCE(override_energy_uwh);
		if (ov_e > 0)
			val->intval = ov_e;
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		ov_e = READ_ONCE(override_energy_uwh);
		if (ov_e > 0) {
			union power_supply_propval full_val;

			if ((orig(psy, POWER_SUPPLY_PROP_ENERGY_FULL, &full_val) == 0 ||
			     orig(psy, POWER_SUPPLY_PROP_CHARGE_FULL, &full_val) == 0) &&
			    full_val.intval > 0) {
				u64 pct = div64_u64((u64)ov_e * 100ULL + (u64)(full_val.intval / 2),
						    (u64)full_val.intval);
				val->intval = clamp_t(int, (int)pct, 0, 100);
			}
		}
		break;

	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		if (val->intval == POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL) {
			union power_supply_propval cap_val;

			if (tbf_get_property(psy, POWER_SUPPLY_PROP_CAPACITY, &cap_val) == 0) {
				if (cap_val.intval > 15)
					val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
				else if (cap_val.intval > 7)
					val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
			}
		}
		break;

	default:
		break;
	}

	return 0;
}

static int tbf_add_battery(struct power_supply *psy, struct acpi_battery_hook *hook)
{
	struct power_supply_desc *desc;
	int i;

	if (!psy || !psy->desc || !psy->desc->get_property)
		return 0;

	desc = (struct power_supply_desc *)psy->desc;
	if (READ_ONCE(desc->get_property) == tbf_get_property)
		return 0;

	mutex_lock(&hook_lock);
	for (i = 0; i < MAX_HOOKED_BATTERIES; i++) {
		if (!hooked_bats[i].psy) {
			hooked_bats[i].desc = desc;
			WRITE_ONCE(hooked_bats[i].orig_get_property, desc->get_property);
			WRITE_ONCE(hooked_bats[i].psy, psy);
			WRITE_ONCE(desc->get_property, tbf_get_property);
			pr_info("thinkpad_battery_fix_kmod: hooked %s\n", desc->name ?: "BAT");
			break;
		}
	}
	mutex_unlock(&hook_lock);
	return 0;
}

static int tbf_remove_battery(struct power_supply *psy, struct acpi_battery_hook *hook)
{
	int i;
	bool unhooked = false;

	mutex_lock(&hook_lock);
	for (i = 0; i < MAX_HOOKED_BATTERIES; i++) {
		if (hooked_bats[i].psy == psy) {
			if (hooked_bats[i].desc && hooked_bats[i].orig_get_property)
				WRITE_ONCE(hooked_bats[i].desc->get_property,
					   hooked_bats[i].orig_get_property);
			WRITE_ONCE(hooked_bats[i].psy, NULL);
			WRITE_ONCE(hooked_bats[i].orig_get_property, NULL);
			hooked_bats[i].desc = NULL;
			unhooked = true;
			pr_info("thinkpad_battery_fix_kmod: unhooked %s\n",
				(psy && psy->desc && psy->desc->name) ? psy->desc->name : "BAT");
			break;
		}
	}
	mutex_unlock(&hook_lock);

	if (unhooked)
		synchronize_rcu();

	return 0;
}

static struct acpi_battery_hook tbf_hook = {
	.name = "ThinkPad Battery 0W/7% Latch Hook",
	.add_battery = tbf_add_battery,
	.remove_battery = tbf_remove_battery,
};

static int __init tbf_kmod_init(void)
{
	battery_hook_register(&tbf_hook);
	pr_info("thinkpad_battery_fix_kmod: loaded\n");
	return 0;
}

static void __exit tbf_kmod_exit(void)
{
	battery_hook_unregister(&tbf_hook);
	synchronize_rcu();
	pr_info("thinkpad_battery_fix_kmod: unloaded\n");
}

module_init(tbf_kmod_init);
module_exit(tbf_kmod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Carlo De Gregorio <carlodegregorio05@gmail.com>");
MODULE_DESCRIPTION("ThinkPad EC 7%/0W battery latch sysfs power_supply override hook");
MODULE_VERSION("1.1.0");
