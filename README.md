# thinkpad-battery-fix

> **Universal Linux workaround & recovery daemon for Lenovo ThinkPad batteries that suddenly drop from ~20–30% to 7%, freeze battery percentage updates, and report 0 W on Linux (while working normally on Windows).**

[🇮🇹 Leggi il riepilogo in italiano](#-italiano)

---

## Symptoms

On affected ThinkPad models running any Linux distribution (tested on ThinkPad T480s `20L8`, applicable to T480 / T470 / X270 / T580 PowerBridge & similar Lenovo EC platforms):

1. Battery discharges normally until around **~20–30%**, then suddenly drops straight to **7%**.
2. From that moment on, reported wattage (`power_now` / `energy-rate`) stays at **0 W** both on battery and while plugged in.
3. While plugged in, status shows *Charging / Plugged* at **0 W** and the battery percentage stays **frozen at 7%** indefinitely—even though the battery is physically charging.
4. The moment you **unplug** the charger, the battery percentage jumps/updates **once** (e.g. from 7% → 13% → 18%) and freezes again at 0 W; plugging back in updates once and freezes again.
5. Charging for a bit and doing a **full reboot** restores normal readings until the next discharge hits ~20–30%.
6. **Does not happen on Windows.**

---

## Root Cause Analysis

Inspection of historical `UPower` telemetry, the ThinkPad ACPI DSDT, the Linux kernel ACPI battery driver (`drivers/acpi/battery.c`), and `upowerd` reveals three interacting layers:

1. **BMS Hardware EDV2 Threshold (7% @ ~9.65 V) & Coulomb Counter Drift**  
   When slow or trickle charging terminates prematurely at a lower pack voltage (e.g. ~12.56 V instead of full ~13.14 V on a 3S pack), the fuel gauge overestimates remaining capacity. During a subsequent load (~20 W) around ~20–31% displayed charge, pack voltage sags to the cell **EDV2** (End-of-Discharge Voltage 2) hardware trip (~9.650 V / ~3.216 V per cell). The BMS snaps `RemainingCapacity` (`SBRC`) to **7%** and zeroes `SBAC` (`power_now = 0 W`).

2. **ThinkPad EC Register Bank (`HIID`) & `_BTP` (`AJTP`) Mismatch on Linux vs. Windows**  
   In the ThinkPad DSDT:
   - EC battery registers (`0xA0..0xAF`: `SBRC`, `SBVO`, `SBAC`) are bank-switched via `HIID` (`0x81`).
   - Regular Linux polls evaluate `_BST` (`GBST`), which only writes `HIID = 0x00`—unlike `_BIX` (`GBIX`), which sweeps `HIID` across pages `1 → 0 → 2 → 6 → 4 → 5`.
   - `_BTP` (`Method (AJTP)`) programs the EC trip-point comparator (`HT0L`/`HT0H` at `0x2B..0x2C`, enabled by bit 4 of `HAM4`) to `SBRC + 1` while charging (`HB0S & 0x20`) or `SBRC - 1` while discharging (`HB0S & 0x40`).
   - Windows (`CmBatt.sys` + Lenovo PM) re-evaluates `_BTP` dynamically on state updates. Mainline Linux (`drivers/acpi/battery.c`) only evaluates `_BIX` and `_BTP` at boot, resume, or ACPI `0x81` notify—leaving `BAT0` page-0 values stale after the 7% EDV2 trip until an AC plug/unplug (`state = pending-charge`) or reboot.

3. **`upowerd` Software Rate Latch (`has_energy_rate_from_hw`)**  
   In `upowerd` (`up_device_supply_battery_refresh`), once `power_now > 0.01 W` has been observed at least once since `upowerd` started, `priv->has_energy_rate_from_hw` is latched to `1` for the lifetime of the process. When `power_now` subsequently drops to `0` at 7%, `upowerd` sees that flag and skips its `d(energy_now)/dt` software wattage estimation, displaying `0.0 W` until restarted.

---

## How `thinkpad-battery-fix` Solves It

`thinkpad-battery-fix` pairs a lightweight kernel `power_supply` hook module (`thinkpad_battery_fix_kmod`) with a background daemon (10 s idle poll / 2 s active latch poll + `udev` `SIGUSR1` wakeup):

- **Auto-detects batteries & units**: Works with single-battery ThinkPads (`BAT0`) and dual-battery PowerBridge models (`BAT0` + `BAT1`), supporting both `energy_*`/`power_now` (µWh/µW) and `charge_*`/`current_now` (µAh/µA) sysfs interfaces.
- **Kernel sysfs hook (`thinkpad_battery_fix_kmod`)**: Registers via `battery_hook_register()` to intercept `power_supply_desc->get_property` while keeping hardware readings visible in `/sys/module/thinkpad_battery_fix_kmod/parameters/raw_*`:
  - Replaces zeroed `power_now` / `current_now` whenever the EC latches `SBAC = 0` after a 7% EDV2 trip, so `btop`, `upowerd`, desktop power panels, and `powertop` immediately read live wattage (`W`) and `time to full`.
  - Masks transient `Not charging` (`pending-charge`) during EC SMBus refresh pulses (`mask_not_charging`) so desktop UI widgets keep showing `Charging` instead of flashing a charge threshold state.
  - Smooths `energy_now` / `capacity` during `Discharging` if the EC freezes `SBRC` while on battery, and normalizes stuck `capacity_level = Critical` once percentage rises above critical levels.
- **Stage 1 recovery** (triggered once per 0 W episode with cooldown):
  1. Kicks ACPI `_BTP` (`AJTP`) via `/sys/class/power_supply/BAT*/alarm`.
  2. While `Charging` (if `charge_behaviour` exposes `inhibit-charge` and `auto`), performs a brief (~1 s) EC SMBus pulse (`inhibit-charge` → `auto`) while masking `Not charging`, forcing the ThinkPad EC to flush cached `SBRC`/`SBVO` registers **without** unbinding `BAT*` from sysfs.
  3. Restarts `upowerd` (supporting `systemd`, `OpenRC`, `runit`, `SysVinit`, and D-Bus activation) to clear any stale 0 W state, then emits a `change` uevent on `/sys/class/power_supply/BAT*/uevent`.
- **Stage 2 continuous telemetry & refresh** (while hardware `raw_power_uw == 0`):
  - **2 s telemetry loop**: Measures system load via Intel RAPL (`package-0`, `dram`) + panel backlight and smooths real battery `dE/dt` rate (while charging) or virtual coulomb discharge (while discharging), pushing live values into `override_power_uw` / `override_energy_uwh`.
  - **Every 30 s** while `Charging`: Re-arms `_BTP` (`alarm`), runs the masked `charge_behaviour` EC SMBus refresh pulse, and triggers batched `BAT*/uevent` `change` syncs every 10–15 s for `upowerd`.
  - Note: kernel driver `unbind`/`bind` is never triggered automatically while `BAT*` is present; it remains available manually via `recover --rebind`.

---

## Installation (All Linux Distributions)

### Prerequisites

Requires `python3` (standard library only), `upower`, `udev`, and kernel headers + `make` / `gcc` (optional but strongly recommended so `thinkpad_battery_fix_kmod` compiles cleanly).

<details>
<summary>Optional package install check by distro</summary>

- **Ubuntu / Debian / Mint / Pop!_OS**:
  ```bash
  sudo apt update && sudo apt install -y python3 upower git build-essential linux-headers-$(uname -r)
  ```
- **Fedora / RHEL / Nobara**:
  ```bash
  sudo dnf install -y python3 upower git gcc make kernel-devel-$(uname -r)
  ```
- **Arch Linux / Manjaro / EndeavourOS / Omarchy**:
  ```bash
  sudo pacman -S --needed python upower git base-devel linux-headers
  ```
- **openSUSE Tumbleweed / Leap**:
  ```bash
  sudo zypper install -y python3 upower git gcc make kernel-default-devel
  ```
- **Alpine / Void / Gentoo (non-systemd)**:
  Install `python3`, `upower`, `build-base`/`gcc`/`make`, and kernel headers, run `sudo ./install.sh`, then start `/usr/local/bin/thinkpad-battery-fix daemon` from your init supervisor (OpenRC / runit / SysVinit).

</details>

### Quick Install

```bash
git clone https://github.com/Carloj005/thinkpad-battery-fix.git
cd thinkpad-battery-fix
sudo ./install.sh
```

Or via `make`:

```bash
sudo make install
```

### Arch Linux Package (`PKGBUILD`)

A ready-to-use Arch package recipe is available in [`packaging/arch/PKGBUILD`](packaging/arch/PKGBUILD):

```bash
cd packaging/arch
makepkg -si
sudo systemctl enable --now thinkpad-battery-fix.service
```

### Uninstall

```bash
sudo ./uninstall.sh
```

---

## CLI Usage

```bash
# Check current battery metrics, resolved ACPI driver, and 0W/7% latch state
thinkpad-battery-fix status

# Target a specific battery on dual-battery ThinkPads
thinkpad-battery-fix -b BAT1 status

# Trigger manual immediate recovery
sudo thinkpad-battery-fix recover

# Force driver rebind + upowerd restart even if power_now != 0
sudo thinkpad-battery-fix recover --rebind

# Check daemon status and logs (systemd)
systemctl status thinkpad-battery-fix.service
journalctl -u thinkpad-battery-fix.service -f
```

---

## Reducing 7% Jumps at the Source (BMS Calibration)

While `thinkpad-battery-fix` prevents the 0 W / frozen percentage lockup automatically, calibrating your battery gauge helps realign the BMS coulomb counter so it doesn't hit EDV2 prematurely at 20–30%:

1. Charge to **100%** using a standard charger and keep it plugged in for **1–2 extra hours** so voltage reaches full top-off (~13.1 V on 3S packs).
2. Unplug and discharge continuously down to ~5% / critical shutoff (avoiding long sleeps mid-discharge).
3. Plug back in and charge uninterrupted to **100%**.

---

## 🇮🇹 Italiano

Fix universale per distribuzioni Linux dedicato al bug dei Lenovo ThinkPad in cui la batteria:
- scende normalmente fino a circa **20–30%** e poi salta improvvisamente al **7%**;
- mostra **0 W** sia in carica sia a batteria;
- rimane congelata al **7%** durante la ricarica finché non si stacca/riattacca fisicamente l'alimentatore o non si riavvia il PC;
- funziona normalmente su **Windows**.

### Installazione rapida
```bash
git clone https://github.com/Carloj005/thinkpad-battery-fix.git
cd thinkpad-battery-fix
sudo ./install.sh
```

### Comandi principali
```bash
thinkpad-battery-fix status        # Mostra stato batteria, driver ACPI ed eventuale blocco 0W/7%
sudo thinkpad-battery-fix recover  # Sblocco manuale immediato
journalctl -u thinkpad-battery-fix.service -n 50 --no-pager
```

---

## License

[MIT](LICENSE)
