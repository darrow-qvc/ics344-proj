# nobash
# A finished job's slot is reused by a newer job.  `jobs` must list in
# job-id order with exactly one '+' and one '-', even while it frees
# the finished entries it reports.
sleep 0.1 &
sleep 1 &
sleep 0.3
jobs
sleep 0.1 &
sleep 0.3
jobs
jobs
exit
