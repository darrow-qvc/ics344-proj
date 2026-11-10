# multi-stage pipelines
echo hello | tr a-z A-Z
printf 'b\na\nb\n' | sort | uniq
printf 'b\na\nb\n' | sort | uniq | wc -l
cat nosuchfile.txt | true
echo 1 | cat | cat | cat | cat | cat | cat | cat
seq 1 100 | grep 5 | sort -r | head -3
# exit status is that of the LAST command
true | false
exit
