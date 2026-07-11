# Convert raw battery sysfs readings into the `sample,watts` CSV that power_adjudicate.awk grades.
#
# Invoked as:  awk -F, -v method=<power_now|vi> -f scripts/lib/power_watts.awk
# stdin:       sample,f1,f2   -- microunits straight out of sysfs, unparsed
# stdout:      sample,watts   -- watts EMPTY when the reading is unusable
#
# Two facts forced this file into existence, both learned on a Deck on 2026-07-10:
#
#   1. **The OLED (Galileo) has no `power_now`.** Its fuel gauge is charge-based: `charge_now` (uAh),
#      `current_now` (uA), `voltage_now` (uV) -- and nothing else. `find /sys -name power_now` comes
#      back empty. power.sh globbed `BAT*/power_now`, found nothing, and exited 3 forever, so the P4
#      battery gate could not run at all on the only unit this project has ever tested on.
#
#   2. **Watts must therefore be P = V x I**, and that multiply must live somewhere a test can reach.
#      Doing it in the sampling loop on the far side of an SSH pipe means the one line that decides
#      what the gate measures is the one line no test ever executes.
#
# An unusable reading emits an EMPTY watts field on purpose. power_adjudicate.awk treats empty as
# "measurement void" and refuses the whole run, because awk coerces "" to 0 and a run that read
# nothing would otherwise average 0.00 W and sail through a `<= 9 W` gate (finding F1).
NR == 1 && $1 == "sample" { next }                 # tolerate a header if one is ever piped in

{
    sample = $1
    watts = ""

    if (method == "power_now") {
        # f1 = power_now, microwatts.
        if ($2 ~ /^[0-9]+$/ && $2 + 0 > 0) watts = $2 / 1000000.0
    } else if (method == "vi") {
        # f1 = voltage_now (uV), f2 = current_now (uA).  uV * uA = 1e-12 W.
        # Both must be positive: a zero current is a battery that is neither charging nor draining,
        # which on a live Deck means the reading is bogus, not that the machine uses no power.
        if ($2 ~ /^[0-9]+$/ && $3 ~ /^[0-9]+$/ && $2 + 0 > 0 && $3 + 0 > 0)
            watts = ($2 + 0) * ($3 + 0) / 1000000000000.0
    } else {
        printf "power_watts.awk: unknown method %s\n", method > "/dev/stderr"
        exit 3
    }

    if (watts == "")
        printf "%s,\n", sample
    else
        printf "%s,%.2f\n", sample, watts
}
