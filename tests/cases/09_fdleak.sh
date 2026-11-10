# every stage of a pipeline must see exactly fds 0,1,2 (+3 = ls's own dirfd)
ls /proc/self/fd
ls /proc/self/fd | cat
echo | ls /proc/self/fd | cat
echo | cat | ls /proc/self/fd | cat | cat
echo | cat | cat | cat | ls /proc/self/fd
ls /proc/self/fd > fds.txt < /dev/null
cat fds.txt
echo | ls /proc/self/fd > fds.txt
cat fds.txt
exit
