# built-ins inside a pipeline run in a child and do not affect the shell
cd / | cat
pwd | grep -c -x /
exit 0 | cat
echo still here
exit 4
