# Convert raw battery sysfs readings into the `sample,watts` CSV that power_adjudicate.awk grades.
#
# Invoked as:  awk -F, -v method=<power_now|vi> -f scripts/lib/power_watts.awk
# stdin:       sample,f1,f2   -- microunits straight out of sysfs, unparsed
# stdout:      sample,watts   -- watts EMPTY when the reading is unusable (voids the run; never 0)
# method=vi computes P = V x I from voltage_now/current_now, for gauges without `power_now` (OLED).
NR == 1 && $1 == "sample" { next }                 # tolerate a header

{
    sample = $1
    watts = ""

    if (method == "power_now") {
        # f1 = power_now, microwatts.
        if ($2 ~ /^[0-9]+$/ && $2 + 0 > 0) watts = $2 / 1000000.0
    } else if (method == "vi") {
        # f1 = voltage_now (uV), f2 = current_now (uA).  uV * uA = 1e-12 W.
        # Zero current on a live Deck is a bogus reading, not zero draw.
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
