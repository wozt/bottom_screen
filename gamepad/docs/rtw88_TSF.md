# rtw88_TSF

A fork of the [rtw88 downstream driver](README.rtw88.md) that exposes the 802.11 **TSF**
(Timing Synchronization Function) counter to userspace.

The TSF is the MAC's free-running 64-bit microsecond clock, shared by every station on a
BSS. Userspace normally has no way to read it. This fork adds that, because projects that
speak a proprietary Wi-Fi protocol on top of a normal adapter — notably
[libdrc](https://github.com/GaryOderNichts/libdrc) and the Wii U GamePad protocol — need to
timestamp against the same clock the hardware uses.

Tested on an **RTL8821CU** USB adapter. The sysfs interface is USB-only; the debugfs one is
chip-independent but unusable under Secure Boot (see below).

---

## Interfaces

### `/sys/class/net/<iface>/device/tsf` — binary, USB only

8 bytes, little-endian `u64`, microseconds. World-readable, no root needed.

This is exactly the path libdrc looks for first (`src/tsf-linux.cpp`), so no adaptation is
required on its side; its fallback is `/sys/class/net/<iface>/tsf`, which is where the old
`memahaxx/drc-mac80211` patch put it.

```console
$ sudo od -An -tu8 /sys/class/net/wlxe0ad474070d8/device/tsf
              5138185
```

Returns an error rather than a bogus value when the counter cannot be read:

| errno | Meaning |
|---|---|
| `EAGAIN` | MAC is powered down — the counter is not running. See [Keeping the counter alive](#keeping-the-counter-alive). |
| `EIO` | Registers answered `0xEAEAEAEA` despite the MAC being marked on. |
| `ENODEV` | Device went away mid-read. |

### `/sys/kernel/debug/ieee80211/phy*/rtw88/tsf` — text, read/write

```console
$ sudo cat /sys/kernel/debug/ieee80211/phy0/rtw88/tsf
0x00000000004e6b09

$ echo 0x1000000 | sudo tee /sys/kernel/debug/ieee80211/phy0/rtw88/tsf   # set
$ echo reset     | sudo tee /sys/kernel/debug/ieee80211/phy0/rtw88/tsf   # zero it
```

Same power-state handling as the sysfs attribute: `EAGAIN` when the MAC is down, `EINVAL` on
an unparseable write.

Writing the TSF fights the hardware's own beacon synchronization once you are associated — it
is a debugging aid, not a way to discipline the clock.

> **Blocked under Secure Boot.** With Secure Boot on, the kernel runs in lockdown `integrity`
> mode, and lockdown denies *all* debugfs access — even as root, you get `EPERM`:
>
> ```console
> $ cat /sys/kernel/security/lockdown
> none [integrity] confidentiality
> $ sudo cat /sys/kernel/debug/ieee80211/phy0/rtw88/tsf
> cat: ...: Operation not permitted
> ```
>
> This is why the sysfs attribute exists. On such a machine, use `/sys/class/net/.../tsf`.
> The debugfs path above is consequently **untested on the development machine** — it mirrors
> the sysfs logic but has only been verified to compile.

---

## Keeping the counter alive

**This is the part that will bite you.**

The TSF registers (`0x0560`/`0x0564`) live in what Realtek calls the *off* section of the
register map: everything outside `0x00..0xFF` and `0x1000..0x10FF` loses power when the MAC
is powered down. When mac80211 declares the hardware idle, rtw88 enters IPS (Inactive Power
Save) and cuts that power. Two things then happen:

1. Every read comes back as **`0xEAEAEAEA`** — the classic Realtek "chip is off" pattern.
2. **The counter is reset to zero.** When the MAC comes back, the TSF restarts from 0.

The second one is fatal for anything doing timing, and no amount of filtering the
`0xEAEAEAEA` reads fixes it. The MAC has to stay powered.

So this fork adds a module parameter:

```console
$ echo 'options rtw_core disable_ips=Y' | sudo tee /etc/modprobe.d/rtw88-tsf.conf
```

It blocks IPS entry from the two *idle* paths only (`rtw_ops_config()` and `rtw_ips_work()`).
The firmware-recovery path still power-cycles the chip normally — it relies on
`rtw_enter_ips()` to do that before `ieee80211_restart_hw()`, so gating the function itself
would break crash recovery.

It can also be toggled at runtime, which is handy for confirming a diagnosis:

```console
$ echo N | sudo tee /sys/module/rtw_core/parameters/disable_ips
```

### The interface still has to be admin-UP

`disable_ips` stops IPS. It does **not** stop the normal driver shutdown: `ip link set <iface>
down` makes mac80211 call `stop()`, which powers the MAC off and gets you `EAGAIN` again.
That is correct behaviour — you do not want the radio powered while the driver is stopped —
but it means the interface must be up. It does *not* need to be associated.

Do not trust `ip -br addr` here: it prints the **operstate**, so an up-but-unassociated Wi-Fi
interface shows as `DOWN`. Check the admin flag instead:

```console
$ cat /sys/class/net/wlxe0ad474070d8/flags
0x1003          # IFF_UP is set
```

---

## Install

Build and load directly:

```console
$ make -j$(nproc)
$ sudo make install
$ echo 'options rtw_core disable_ips=Y' | sudo tee /etc/modprobe.d/rtw88-tsf.conf
$ sudo modprobe -r rtw_8821cu rtw_8821c rtw_usb rtw_core
$ sudo modprobe rtw_8821cu
```

With Secure Boot enabled, use `sudo make sign-install` instead of `make install`, or install
through DKMS so it is signed with your enrolled MOK. If you already have a DKMS entry, note
that `dkms install --force` will happily reinstall a stale build — remove it first:

```console
$ sudo dkms remove rtw88/<version> -k $(uname -r)
$ sudo dkms install rtw88/<version> -k $(uname -r)
```

See [README.rtw88.md](README.rtw88.md) for prerequisites, supported chipsets and general
rtw88 build instructions.

---

## Verifying

```console
$ sudo ip link set wlxe0ad474070d8 up
$ while true; do
    printf '%s ' "$(date '+%H:%M:%S.%3N')"
    sudo od -An -tu8 /sys/class/net/wlxe0ad474070d8/device/tsf
    sleep 0.1
  done
```

The value must increase monotonically, at roughly 1 000 000 per second of wall time, with no
`16927600444109941482` (that is `0xEAEAEAEA...` read as decimal) and no jump back to a small
number.

Measured on an RTL8821CU, unassociated, interface up:

| | |
|---|---|
| 2781 reads over 150 s at 20 Hz | 0 errors, 0 dead reads, 0 regressions |
| Drift vs. NTP-disciplined host clock over 60 s | below measurement resolution |
| Read latency (n=300) | median 431 µs, p95 467 µs, max 9.3 ms |

---

## Accuracy and limits

Each read costs **two USB control transfers** (~215 µs each), because the counter's low and
high halves are separate registers. The value you get is therefore latched somewhere inside a
~430 µs window, so treat the timestamp as **±200 µs**. That is fine for beacon-scale
synchronization; it is not fine if you need sub-100 µs certainty.

The low half wraps every ~71 minutes. The driver re-reads the high half when the low half is
below one second, so a wrap landing between the two transfers cannot produce a 2³² µs jump.

If you eventually need better than ±200 µs, the right move is not to poll faster — it is to
take the TSF the hardware already stamps into the RX descriptor of each received frame, which
costs no bus round-trip at all. That is not implemented here.

Keeping the MAC powered while idle costs power. `disable_ips=Y` is for machines doing this
kind of work, not for laptops on battery.

---

## What changed vs. upstream rtw88

| File | Change |
|---|---|
| `usb.c` | `rtw_usb_read_tsf()` + the `tsf` binary sysfs attribute, with power-state check, wrap guard and fixed-endian output |
| `debug.c` | `tsf` debugfs read/write, same power-state handling |
| `main.c`, `main.h` | `disable_ips` module parameter, `RTW_REG_DEAD` |
| `mac80211.c`, `main.c` | `disable_ips` gate on the two idle IPS entry points |
| `reg.h` | `REG_TSFTR` / `REG_TSFTR_HIGH` |

---

## Licence

GPL-2.0 OR BSD-3-Clause, same as rtw88.
