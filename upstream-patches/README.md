# Upstream Patches

Standalone upstream-ready patches derived from the root-cause analysis of the Lenovo ThinkPad T480s (`20L8`) 7% / 0 W battery latch bug.

Unlike the full `thinkpad-battery-fix` workaround (kernel sysfs override hook + RAPL/`dE/dt` userspace daemon), these patches target the underlying bugs directly in upstream projects without synthetic userspace telemetry.

## 1. UPower (`gitlab.freedesktop.org/upower/upower`)

- **File**: [`0001-battery-Fall-back-to-power-estimation-when-hardware-.patch`](0001-battery-Fall-back-to-power-estimation-when-hardware-.patch)
- **Target**: `src/up-device-battery.c`, `src/linux/integration-test.py` (based on `master` commit `bc6b3200b28d`)
- **Problem**: Once hardware reports `energy.rate > 0.01 W` at least once, `UpDeviceBattery` permanently latches `priv->trust_power_measurement = TRUE` and never runs `up_device_battery_estimate_power()` again, leaving `energy-rate`, `time-to-full`, and `time-to-empty` stuck at `0` whenever the EC/BMS latches `SBAC` to `0`.
- **Fix**: Track `last_hw_power_measurement` and fall back to `dE/dt` estimation whenever hardware reports `<= 0.01 W` outside the post-discontinuity distrust window, ignoring ring-buffer samples at or before `last_hw_power_measurement` so true idle transitions stay `0 W`. Direct hardware readings resume immediately if `power_now` becomes non-zero again.
- **Verification**: Compiled `up-device-battery.o` with `-Wall -Wextra -Werror` (0 warnings/errors); 5-case C unit harness verified normal rate, 0 W idle, 0 W charging fallback, discontinuity window, and hardware recovery; `integration-test.py` passed `py_compile`.

### Submit to UPower GitLab
1. Fork `https://gitlab.freedesktop.org/upower/upower` on GitLab (`gitlab.freedesktop.org`).
2. Apply and push:
   ```bash
   git clone https://gitlab.freedesktop.org/<YOUR_USER>/upower.git
   cd upower
   git checkout -b fix/estimate-rate-when-hw-drops-to-zero
   git am /path/to/0001-battery-Fall-back-to-power-estimation-when-hardware-.patch
   git push -u origin fix/estimate-rate-when-hw-drops-to-zero
   ```
3. Open the Merge Request via the URL returned by GitLab.

---

## 2. Linux Kernel (`drivers/acpi/battery.c`) — `[PATCH RFC]`

- **File**: [`0001-ACPI-battery-Re-arm-_BTP-alarm-on-ThinkPad-T480s-sta.patch`](0001-ACPI-battery-Re-arm-_BTP-alarm-on-ThinkPad-T480s-sta.patch)
- **Target**: `drivers/acpi/battery.c` (based on `torvalds/linux` commit `4405c18c95d2`)
- **Problem**: On ThinkPad T480s (`20L8`), DSDT `_BTP` calls `Method (AJTP)`, which programs EC trip-point comparator registers (`HT0L`/`HT0H`) relative to `SBRC` (`SBRC + 1` while charging, `SBRC - 1` while discharging) and enables `HAM4 |= 0x10`. Linux currently only evaluates `_BTP` on probe/resume or sysfs writes, so the comparator goes stale across state transitions or after a 7% EDV2 drop.
- **Fix**: Adds a `LENOVO` / `20L8` DMI quirk (`battery_rearm_alarm`) in `acpi_battery_get_state()` that re-arms `_BTP` on battery state changes or when `rate_now == 0` during charging/discharging and `rate_now` just dropped to `0` or `capacity_now` changed.
- **Verification**: `scripts/checkpatch.pl --strict --no-tree` passed with `0 errors, 0 warnings, 0 checks`; `battery.o` compile-tested with `-Werror`.

### Submit via `git send-email`
1. Install SMTP Perl dependencies (Arch Linux: `sudo pacman -S perl-authen-sasl perl-io-socket-ssl`) and configure `[sendemail]` in `~/.gitconfig`.
2. Send to ACPI maintainers:
   ```bash
   git send-email \
     --to="Rafael J. Wysocki <rafael@kernel.org>" \
     --cc="Len Brown <lenb@kernel.org>" \
     --cc="linux-acpi@vger.kernel.org" \
     --cc="linux-kernel@vger.kernel.org" \
     0001-ACPI-battery-Re-arm-_BTP-alarm-on-ThinkPad-T480s-sta.patch
   ```
