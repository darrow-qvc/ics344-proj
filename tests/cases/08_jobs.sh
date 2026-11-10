# nobash   (bash does not announce background jobs identically here)
sleep 0.3 &
jobs
sleep 0.6
jobs
jobs
sleep 0.2 &
sleep 0.2 &
jobs
sleep 0.5
jobs
true &
sleep 0.2
jobs
false &
sleep 0.2
jobs
fg %9
bg %9
fg
bg
exit
