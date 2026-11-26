#!/bin/sh
# Test suite for cshell. Run: make test
set -u
cd "$(dirname "$0")"

CS=./cshell
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pass=0
fail=0

ok() { pass=$((pass + 1)); printf 'ok   - %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf 'FAIL - %s\n' "$1"; }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (want [$2] got [$3])"; fi; }

# run cshell with a script on stdin, return stdout
run() { "$CS"; }

check "echo" "hello world" "$(run <<'EOF'
echo hello world
EOF
)"

check "echo -n" "abc" "$(run <<'EOF'
echo -n abc
EOF
)"

check "single quotes" "a b" "$(run <<'EOF'
echo 'a b'
EOF
)"
check "double quotes" "a b" "$(run <<'EOF'
echo "a b"
EOF
)"
check "backslash escape" "a b" "$(run <<'EOF'
echo a\ b
EOF
)"
check "empty quotes" "x  y" "$(run <<'EOF'
echo x '' y
EOF
)"

check "false && echo" "" "$(run <<'EOF'
false && echo nope
EOF
)"
check "false || echo" "yes" "$(run <<'EOF'
false || echo yes
EOF
)"
check "true && echo" "yes" "$(run <<'EOF'
true && echo yes
EOF
)"
check "exit status propagation" "0" "$(run <<'EOF'
false; true
EOF
)"; st=$?; check "last status" "0" "$st"

check "pipe" "3" "$(run <<'EOF'
printf 'a\nb\nc\n' | wc -l
EOF
)"
check "multi pipe" "1" "$(run <<'EOF'
printf 'a\nb\nc\n' | grep b | wc -l
EOF
)"
check "pipe no spaces" "3" "$(run <<'EOF'
printf 'a\nb\nc\n'|wc -l
EOF
)"

printf 'x\n' > "$TMP/in.txt"
check "input redirect" "x" "$(run <<EOF
cat < $TMP/in.txt
EOF
)"
check "input redirect no spaces" "x" "$(run <<EOF
cat <$TMP/in.txt
EOF
)"

run > "$TMP/out.txt" <<'EOF'
echo redirect
EOF
check "output redirect" "redirect" "$(cat "$TMP/out.txt")"

run >> "$TMP/out.txt" <<'EOF'
append
EOF
check "append redirect" "redirect
append" "$(cat "$TMP/out.txt")"

check "output redirect no spaces" "hi" "$(run >"$TMP/o2.txt" <<'EOF'
echo hi
EOF
cat "$TMP/o2.txt")"

run > "$TMP/err.txt" 2>&1 <<'EOF'
echo oops >&2
EOF
check "stderr merge" "oops" "$(cat "$TMP/err.txt")"

run 2> "$TMP/err2.txt" >/dev/null <<'EOF'
echo bad >&2
EOF
check "stderr file" "bad" "$(cat "$TMP/err2.txt")"

run > "$TMP/err3.txt" 2> "$TMP/err3.txt" <<'EOF'
echo e >&2
EOF
check "2> no spaces" "e" "$(cat "$TMP/err3.txt")"

check "cd and pwd" "$TMP" "$(run <<EOF
cd $TMP
pwd
EOF
)"

check "cd - returns home-ish" "$(dirname "$TMP")" "$(run <<EOF
cd $TMP
cd ..
pwd
EOF
)"

check "cd -" "$TMP" "$(run <<EOF
cd $TMP
cd ..
cd -
pwd
EOF
)"

check "cd no arg" "$(printf '%s' "${HOME:-/root}")" "$(run <<'EOF'
cd
pwd
EOF
)"

check "cd bad dir" "1" "$(run <<'EOF'
cd /nonexistent-dir-xyz
EOF
echo $?)"

check "history count" "2" "$(run <<'EOF'
echo one
echo two
history
EOF
" | wc -l | tr -d ' ')"

check "jobs empty" "0" "$(run <<'EOF'
jobs
EOF
" | wc -l | tr -d ' ')"

check "background echo" "bg" "$(run <<'EOF'
echo bg &
wait
EOF
)"

check "env prefix" "1" "$(run <<'EOF'
FOO=bar sh -c 'echo 1'
EOF
)"

touch "$TMP/a1.txt" "$TMP/a2.txt" "$TMP/b1.txt"
check "glob count" "2" "$(run <<EOF
cd $TMP
echo a*.txt
EOF
" | wc -w | tr -d ' ')"

check "semicolon list" "1 2" "$(run <<'EOF'
echo 1; echo 2
EOF
" | tr '\n' ' ' | sed 's/ $//')"

check "command not found" "127" "$(run <<'EOF'
definitely-not-a-command-xyz
EOF
echo $?)"

cat > "$TMP/s.sh" <<'EOF'
echo script
echo mode
EOF
check "script mode" "script
mode" "$("$CS" "$TMP/s.sh")"

echo
echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]
