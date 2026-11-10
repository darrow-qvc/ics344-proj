# words, arguments, quoting and escaping
echo hello world
echo   spaced    out   
echo 'single quoted  keeps   spaces'
echo "double quoted  keeps   spaces"
echo 'it''s' concatenated
echo "say \"hi\"" and 'a "b" c'
echo back\ slash\ space
echo "backslash \\ inside dq" 'no \escape in sq'
echo a"b c"d
echo ""
echo "" | wc -c
echo pipe\|not\|operator
echo '>' not a redirect
echo trailing comment # this is ignored
echo not#a#comment
exit
