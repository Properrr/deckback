# Adjudicate a power-<sha>.csv (header `sample,watts`) against the P4 draw budget.
#
# Invoked as:  awk -F, -v max=<watts> -f scripts/lib/power_adjudicate.awk <csv>
# Exit codes:  0 PASS · 2 FAIL (mean over budget) · 3 measurement void (no/bad samples)
#
# Split out of power.sh so it can be tested without a Deck (tests/harness/). The behaviour that
# matters and must never regress: an empty or non-numeric watts field is NOT worth zero. awk's
# implicit string->number coercion turns "" into 0, which is how this gate once reported
# `mean 0.00 W` — a perfect score — from a run that read no battery data at all.
NR == 1 { next }                                   # header
{ total++ }
$2 == "" || $2 !~ /^[0-9]+\.[0-9]+$/ { bad++; next }
{ s += $2; n++ }
END {
  if (total == 0) {
    print "error: no samples recorded" > "/dev/stderr"
    exit 3
  }
  if (bad > 0) {
    printf "error: %d/%d samples unreadable - measurement void\n", bad, total > "/dev/stderr"
    exit 3
  }
  mean = s / n
  printf "mean %.2f W over %d samples (gate <= %.2f W)\n", mean, n, max > "/dev/stderr"
  if (mean > max) {
    print "FAIL: mean draw exceeds the gate" > "/dev/stderr"
    exit 2
  }
  print "PASS" > "/dev/stderr"
}
