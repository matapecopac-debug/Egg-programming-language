#!/bin/bash
set -e

BOLD="\033[1m"
CYAN="\033[96m"
GREEN="\033[92m"
RED="\033[91m"
RESET="\033[0m"

echo -e "${BOLD}${CYAN}Installing gutterball v5...${RESET}"

[ "$(uname -m)" = "x86_64" ] || { echo -e "${RED}Needs x86_64${RESET}"; exit 1; }

# Install compiler
echo -e "${CYAN}Installing compiler...${RESET}"
sudo cp gutterball /usr/local/bin/gutterball
sudo chmod 755 /usr/local/bin/gutterball

# Create library directory
echo -e "${CYAN}Creating library directory...${RESET}"
sudo mkdir -p /usr/local/lib/vex

# Write all libraries directly using sudo tee
echo -e "${CYAN}Installing libraries...${RESET}"

sudo tee /usr/local/lib/vex/io.egg > /dev/null << 'EOLIB'
fn print(s: ptr) -> void {
    execute.Write(s)
}
fn println(s: ptr) -> void {
    execute.WriteIn(s)
}
fn printInt(n: i64) -> void {
    execute.WriteInt(n)
}
fn printChar(c: i64) -> void {
    execute.WriteChar(c)
}
fn printLine() -> void {
    execute.WriteIn("")
}
fn readInt() -> i64 {
    return execute.ReadInt()
}
fn readLine(buf: ptr, max: i64) -> ptr {
    execute.ReadIn(buf, max)
    return buf
}
fn ask(prompt: ptr, buf: ptr, max: i64) -> ptr {
    execute.Write(prompt)
    execute.ReadIn(buf, max)
    return buf
}
fn askInt(prompt: ptr) -> i64 {
    execute.Write(prompt)
    return execute.ReadInt()
}
fn printBool(b: i64) -> void {
    if b {
        execute.WriteIn("true")
    } else {
        execute.WriteIn("false")
    }
}
fn banner(text: ptr) -> void {
    execute.WriteIn("================================")
    execute.Write("  ")
    execute.WriteIn(text)
    execute.WriteIn("================================")
}
fn hr() -> void {
    execute.WriteIn("--------------------------------")
}
EOLIB
echo -e "  ${GREEN}✓${RESET} io.egg"

sudo tee /usr/local/lib/vex/math.egg > /dev/null << 'EOLIB'
fn abs(n: i64) -> i64 {
    if n < 0 { return -n }
    return n
}
fn max(a: i64, b: i64) -> i64 {
    if a > b { return a }
    return b
}
fn min(a: i64, b: i64) -> i64 {
    if a < b { return a }
    return b
}
fn clamp(val: i64, lo: i64, hi: i64) -> i64 {
    if val < lo { return lo }
    if val > hi { return hi }
    return val
}
fn pow(base: i64, exp: i64) -> i64 {
    if exp == 0 { return 1 }
    var result = 1
    var b = base
    var e = exp
    while e > 0 {
        if e % 2 == 1 { result = result * b }
        b = b * b
        e = e / 2
    }
    return result
}
fn sqrt(n: i64) -> i64 {
    if n <= 0 { return 0 }
    if n == 1 { return 1 }
    var x = n
    var y = (x + 1) / 2
    while y < x {
        x = y
        y = (x + n / x) / 2
    }
    return x
}
fn gcd(a: i64, b: i64) -> i64 {
    var aa = a
    var bb = b
    while bb != 0 {
        var t = bb
        bb = aa % bb
        aa = t
    }
    return aa
}
fn lcm(a: i64, b: i64) -> i64 {
    return a / gcd(a, b) * b
}
fn factorial(n: i64) -> i64 {
    if n <= 1 { return 1 }
    return n * factorial(n - 1)
}
fn fibonacci(n: i64) -> i64 {
    if n <= 1 { return n }
    return fibonacci(n - 1) + fibonacci(n - 2)
}
fn is_prime(n: i64) -> i64 {
    if n < 2 { return 0 }
    if n == 2 { return 1 }
    if n % 2 == 0 { return 0 }
    var i = 3
    while i * i <= n {
        if n % i == 0 { return 0 }
        i = i + 2
    }
    return 1
}
fn is_even(n: i64) -> i64 {
    if n % 2 == 0 { return 1 }
    return 0
}
fn is_odd(n: i64) -> i64 {
    if n % 2 != 0 { return 1 }
    return 0
}
fn sign(n: i64) -> i64 {
    if n > 0 { return 1 }
    if n < 0 { return -1 }
    return 0
}
fn digits(n: i64) -> i64 {
    if n == 0 { return 1 }
    var count = 0
    var num = n
    if num < 0 { num = -num }
    while num > 0 {
        count = count + 1
        num = num / 10
    }
    return count
}
fn sum_digits(n: i64) -> i64 {
    var num = n
    if num < 0 { num = -num }
    var s = 0
    while num > 0 {
        s = s + num % 10
        num = num / 10
    }
    return s
}
fn reverse(n: i64) -> i64 {
    var result = 0
    var num = n
    if num < 0 { num = -num }
    while num > 0 {
        result = result * 10 + num % 10
        num = num / 10
    }
    return result
}
EOLIB
echo -e "  ${GREEN}✓${RESET} math.egg"

sudo tee /usr/local/lib/vex/string.egg > /dev/null << 'EOLIB'
fn len(s: ptr) -> i64 {
    return execute.StrLen(s)
}
fn eq(a: ptr, b: ptr) -> i64 {
    return execute.StrEq(a, b)
}
fn copy(dst: ptr, src_s: ptr) -> void {
    execute.StrCopy(dst, src_s)
}
fn cat(dst: ptr, src_s: ptr) -> void {
    execute.StrCat(dst, src_s)
}
fn new(size: i64) -> ptr {
    var buf = execute.Alloc(size)
    @buf = 0
    return buf
}
fn free(buf: ptr, size: i64) -> void {
    execute.Free(buf, size)
}
fn empty(s: ptr) -> i64 {
    var first = @s
    if first == 0 { return 1 }
    return 0
}
fn contains(s: ptr, ch: i64) -> i64 {
    var p = s
    loop {
        var c = @p
        if c == 0 { return 0 }
        if c == ch { return 1 }
        p = p + 1
    }
    return 0
}
fn index_of(s: ptr, ch: i64) -> i64 {
    var p = s
    var i = 0
    loop {
        var c = @p
        if c == 0 { return -1 }
        if c == ch { return i }
        p = p + 1
        i = i + 1
    }
    return -1
}
fn to_upper(s: ptr) -> void {
    var p = s
    loop {
        var c = @p
        if c == 0 { break }
        if c >= 97 && c <= 122 {
            @p = c - 32
        }
        p = p + 1
    }
}
fn to_lower(s: ptr) -> void {
    var p = s
    loop {
        var c = @p
        if c == 0 { break }
        if c >= 65 && c <= 90 {
            @p = c + 32
        }
        p = p + 1
    }
}
fn trim_newline(s: ptr) -> void {
    var l = execute.StrLen(s)
    if l == 0 { return }
    var p = s + l - 1
    var last = @p
    if last == 10 {
        @p = 0
    }
}
fn count_char(s: ptr, ch: i64) -> i64 {
    var count = 0
    var p = s
    loop {
        var c = @p
        if c == 0 { break }
        if c == ch { count = count + 1 }
        p = p + 1
    }
    return count
}
fn repeat_char(buf: ptr, ch: i64, times: i64) -> void {
    var p = buf
    var i = 0
    while i < times {
        @p = ch
        p = p + 1
        i = i + 1
    }
    @p = 0
}
fn starts_with(s: ptr, prefix: ptr) -> i64 {
    var sp = s
    var pp = prefix
    loop {
        var pc = @pp
        if pc == 0 { return 1 }
        var sc = @sp
        if sc != pc { return 0 }
        sp = sp + 1
        pp = pp + 1
    }
    return 0
}
fn ends_with(s: ptr, suffix: ptr) -> i64 {
    var slen = execute.StrLen(s)
    var suflen = execute.StrLen(suffix)
    if suflen > slen { return 0 }
    var sp = s + slen - suflen
    var pp = suffix
    loop {
        var pc = @pp
        if pc == 0 { return 1 }
        var sc = @sp
        if sc != pc { return 0 }
        sp = sp + 1
        pp = pp + 1
    }
    return 0
}
EOLIB
echo -e "  ${GREEN}✓${RESET} string.egg"

sudo tee /usr/local/lib/vex/memory.egg > /dev/null << 'EOLIB'
fn alloc(size: i64) -> ptr {
    return execute.Alloc(size)
}
fn free(buf: ptr, size: i64) -> void {
    execute.Free(buf, size)
}
fn alloc_zeroed(size: i64) -> ptr {
    var buf = execute.Alloc(size)
    var i = 0
    while i < size {
        buf[i] = 0
        i = i + 1
    }
    return buf
}
fn copy(dst: ptr, src_p: ptr, bytes: i64) -> void {
    var i = 0
    while i < bytes {
        dst[i] = src_p[i]
        i = i + 1
    }
}
fn zero(buf: ptr, bytes: i64) -> void {
    var i = 0
    while i < bytes {
        buf[i] = 0
        i = i + 1
    }
}
fn fill(buf: ptr, val: i64, count: i64) -> void {
    var i = 0
    while i < count {
        buf[i] = val
        i = i + 1
    }
}
fn realloc(old_buf: ptr, old_size: i64, new_size: i64) -> ptr {
    var new_buf = execute.Alloc(new_size)
    var copy_size = old_size
    if new_size < copy_size { copy_size = new_size }
    var i = 0
    while i < copy_size {
        new_buf[i] = old_buf[i]
        i = i + 1
    }
    execute.Free(old_buf, old_size)
    return new_buf
}
EOLIB
echo -e "  ${GREEN}✓${RESET} memory.egg"

sudo tee /usr/local/lib/vex/sort.egg > /dev/null << 'EOLIB'
fn _sort_and_print(arr: ptr, len: i64) -> void {
    for i in 0..len {
        for j in 0..len - 1 {
            if arr[j] > arr[j + 1] {
                var tmp = arr[j]
                arr[j] = arr[j + 1]
                arr[j + 1] = tmp
            }
        }
    }
    execute.WriteIn("Sorted:")
    for i in 0..len {
        execute.WriteInt(arr[i])
    }
}
fn min(arr: ptr, len: i64) -> i64 {
    var m = arr[0]
    for i in 1..len {
        if arr[i] < m { m = arr[i] }
    }
    return m
}
fn max(arr: ptr, len: i64) -> i64 {
    var m = arr[0]
    for i in 1..len {
        if arr[i] > m { m = arr[i] }
    }
    return m
}
fn asc(arr: ptr, len: i64) -> void {
    for i in 0..len {
        for j in 0..len - 1 {
            if arr[j] > arr[j + 1] {
                var tmp = arr[j]
                arr[j] = arr[j + 1]
                arr[j + 1] = tmp
            }
        }
    }
}
fn desc(arr: ptr, len: i64) -> void {
    for i in 0..len {
        for j in 0..len - 1 {
            if arr[j] < arr[j + 1] {
                var tmp = arr[j]
                arr[j] = arr[j + 1]
                arr[j + 1] = tmp
            }
        }
    }
}
fn is_sorted(arr: ptr, len: i64) -> i64 {
    for i in 0..len - 1 {
        if arr[i] > arr[i + 1] { return 0 }
    }
    return 1
}
fn sum(arr: ptr, len: i64) -> i64 {
    var total = 0
    for i in 0..len {
        total = total + arr[i]
    }
    return total
}
EOLIB
echo -e "  ${GREEN}✓${RESET} sort.egg"

echo ""
echo -e "${GREEN}✓ gutterball v5 fully installed!${RESET}"
echo ""
echo "Compiler:  /usr/local/bin/gutterball"
echo "Libraries: /usr/local/lib/vex/"
echo ""
echo "Usage:     gutterball myfile.egg -o myprogram"
echo "           ./myprogram"
echo ""
echo "Libraries: get-Library/io"
echo "           get-Library/math"
echo "           get-Library/string"
echo "           get-Library/memory"
echo "           get-Library/sort"
