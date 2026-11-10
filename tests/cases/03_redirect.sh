# > >> < and collisions
echo one > out.txt
cat out.txt
echo two >> out.txt
cat out.txt
echo three > out.txt
cat out.txt
sort < out.txt
wc -l < out.txt > count.txt
cat count.txt
cat < missing.txt
echo nothing > /nonexistent/dir/file
# both files are opened in order; a.txt is created empty, b.txt gets the output
echo x > a.txt > b.txt
cat a.txt
cat b.txt
# collision: output truncates the input file before it is read
echo payload > c.txt
cat < c.txt > c.txt
wc -c < c.txt
# redirection wins over a pipe on the same descriptor
echo from-pipe | cat < b.txt
echo to-file | cat > d.txt | cat
cat d.txt
# built-in with redirection, nothing is forked
cd /tmp > e.txt
pwd
exit
