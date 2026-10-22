#!/bin/sh
# Tests packaging/install.sh without root and without network: every run happens in a temporary
# directory (--root, or a temporary HOME), with fake system commands (id, uname, curl, wget, getent,
# systemctl, runuser, sudo, modprobe, udevadm) and a fake GitHub (public download URLs and the API
# that a private repository needs). PATH holds only the fakes and the basic tools the script may use,
# so a test also fails when the script calls something unexpected.
#
#   sh tests/test_install.sh [-v]
# Every case runs under each shell found among: sh, dash, bash --posix, busybox sh (SHELLS="dash" to
# pick). Exit status 0 when every check passed.
# shellcheck disable=SC2329  # the check helpers and cases are called through check / the case list
# shellcheck disable=SC2016  # the fake commands are written with printf: their $ are theirs
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
TOP=${HERE%/*}
INSTALL=$TOP/packaging/install.sh
PKG=$TOP/packaging
VERBOSE=0
[ "${1:-}" = -v ] && VERBOSE=1

W=$(mktemp -d "${TMPDIR:-/tmp}/test-install.XXXXXX") || exit 1
trap 'rm -rf "$W"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

APP=elrs-wifi-joystick
REPO=test/repo
ARCHES="x86_64 i686 aarch64 armv7 armv6 riscv64"
LOG=$W/log          # every fake command call, one line each
CURLCFG=$W/curlcfg  # what curl read with -K - (the token)
OUT=$W/out          # the installer's stdout + stderr
WEB=$W/web          # the fake GitHub: <host>/<path>
RC=0
NCHECK=0
NFAIL=0
CASE=

# ------------------------------------------------------------------ reporting
pass() {
  NCHECK=$((NCHECK + 1))
  [ "$VERBOSE" = 0 ] || printf '  ok  %s: %s\n' "$CASE" "$1"
}
fail() {
  NCHECK=$((NCHECK + 1))
  NFAIL=$((NFAIL + 1))
  printf 'FAIL  [%s] %s: %s\n' "$SHNAME" "$CASE" "$1"
  if [ -f "$OUT" ]; then sed 's/^/      | /' "$OUT" | tail -n 25; fi
}
check() {  # description command...
  ck_d=$1
  shift
  if "$@"; then pass "$ck_d"; else fail "$ck_d"; fi
}
has() { grep -qF -- "$1" "$OUT"; }
hasnt() { ! grep -qF -- "$1" "$OUT"; }
logged() { grep -qF -- "$1" "$LOG"; }
not_logged() { ! grep -qF -- "$1" "$LOG"; }
same() { cmp -s "$1" "$2"; }
# shellcheck disable=SC2012  # the mode string, as people read it
mode_is() { [ "$(ls -ld "$1" | cut -c1-10)" = "$2" ]; }
gone() { [ ! -e "$1" ] && [ ! -L "$1" ]; }
rc_is() { [ "$RC" = "$1" ]; }
empty_dir() { [ -z "$(ls -A "$1")" ]; }

ext() {  # the path of a real tool (not a shell builtin)
  for ex_d in /usr/local/bin /usr/bin /bin /usr/sbin /sbin; do
    if [ -x "$ex_d/$1" ]; then printf '%s\n' "$ex_d/$1"; return 0; fi
  done
  return 1
}

# ------------------------------------------------------------------ the fake system
TOOLS="cat mkdir cp chmod mv rm rmdir ln ls sed awk grep tr cut paste head wc mktemp env sleep"
FAKE=$W/fakebin     # fake system commands
mkdir -p "$FAKE" "$W/nocurl" "$W/wget" "$W/shasum" "$W/tmp"
SHA256SUM=$(ext sha256sum) || { echo "test_install.sh: sha256sum is missing" >&2; exit 1; }

# the basic tools the installer may use: dir, then dir-nosha (the same without sha256sum).
# src "system" = this machine's tools, else a busybox binary (its applets, like Alpine or OpenWrt)
make_sys() {  # dir src
  mkdir -p "$1" "$1-nosha"
  for t in $TOOLS sha256sum; do
    if [ "$2" = system ]; then
      p=$(ext "$t") || { echo "test_install.sh: $t is missing on this machine" >&2; exit 1; }
    else
      p=$2
    fi
    ln -s "$p" "$1/$t"
    [ "$t" = sha256sum ] || ln -s "$p" "$1-nosha/$t"
  done
}
use_sys() {  # dir: the PATHs of the runs
  PATH_STD=$FAKE:$1
  PATH_WGET=$W/nocurl:$W/wget:$1
  PATH_NOSHA=$FAKE:$W/shasum:$1-nosha
  PATH_NOTOOL=$FAKE:$1-nosha
}
make_sys "$W/sys" system
use_sys "$W/sys"

fake() {  # name, then the script body on stdin
  cat > "$FAKE/$1"
  chmod 755 "$FAKE/$1"
}

fake id <<'EOF'
#!/bin/sh
echo "id $*" >> "$FAKE_LOG"
case "$*" in
  -u) echo "$FAKE_UID" ;;
  -un) echo "$FAKE_USER" ;;
  "-u alice") echo 1000 ;;
  "-u bob") echo 1001 ;;
  "-u root") echo 0 ;;
  "-nG bob") echo "bob ${FAKE_BOB_GROUPS:-users}" ;;
  "-nG "*) echo "$2" ;;
  *) echo "id: $*: no such user" >&2; exit 1 ;;
esac
EOF

fake uname <<'EOF'
#!/bin/sh
[ "$1" = -m ] || exit 1
echo "$FAKE_MACHINE"
EOF

fake getent <<'EOF'
#!/bin/sh
echo "getent $*" >> "$FAKE_LOG"
case "$*" in
  "passwd bob") echo "bob:x:1001:1001:Bob:/home/bob:/bin/sh" ;;
  "passwd alice") echo "alice:x:1000:1000:Alice:/home/alice:/bin/sh" ;;
  "group input") [ -z "${FAKE_NO_INPUT:-}" ] && echo "input:x:104:" ;;
  *) exit 2 ;;
esac
EOF

# curl: -o OUT, -K - (config with the token on stdin), -H headers; the URL maps to $FAKE_WEB/<host>/<path>.
# The API (api.github.com) answers only with the right token, an asset only with Accept octet-stream.
fake curl <<'EOF'
#!/bin/sh
{ printf 'curl'; for a in "$@"; do printf ' [%s]' "$a"; done; echo; } >> "$FAKE_LOG"
out= url= cfg= accept=
while [ $# -gt 0 ]; do
  case $1 in
    -o) out=$2; shift ;;
    -K) [ "$2" = - ] && cfg=$(cat) && printf '%s\n' "$cfg" >> "$FAKE_CURLCFG"; shift ;;
    -H) case $2 in Accept:*) accept=${2#Accept: } ;; esac; shift ;;
    --proto|--retry|--connect-timeout) shift ;;
    -*) ;;
    *) url=$1 ;;
  esac
  shift
done
case $url in https://*) ;; *) exit 1 ;; esac
case $url in
  https://api.github.com/*)
    case $cfg in *"Authorization: Bearer $FAKE_TOKEN"*) ;; *) exit 22 ;; esac
    case $url in */releases/assets/*) [ "$accept" = application/octet-stream ] || { echo '{"json":1}' > "$out"; exit 0; } ;; esac
    ;;
esac
f=$FAKE_WEB/${url#https://}
[ -f "$f" ] || exit 22
cp "$f" "$out"
EOF

fake systemctl <<'EOF'
#!/bin/sh
echo "systemctl $* (XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-})" >> "$FAKE_LOG"
case "$*" in *is-active*) echo active ;; esac
exit 0
EOF

for c in modprobe udevadm; do
  printf '#!/bin/sh\necho "%s $*" >> "$FAKE_LOG"\n' "$c" | fake "$c"
done

# runuser -u USER -- cmd..., sudo -u USER -- cmd...: run it (as this user)
for c in runuser sudo; do
  printf '#!/bin/sh\necho "%s $*" >> "$FAKE_LOG"\nshift 3\nexec "$@"\n' "$c" | fake "$c"
done

# wget -q -O OUT URL (public downloads only); shasum -a 256 FILE
printf '#!/bin/sh\n{ printf wget; for a in "$@"; do printf " [%%s]" "$a"; done; echo; } >> "$FAKE_LOG"\nf=$FAKE_WEB/${4#https://}\n[ -f "$f" ] || exit 8\ncp "$f" "$3"\n' > "$W/wget/wget"
printf '#!/bin/sh\necho "shasum $*" >> "$FAKE_LOG"\n[ "$1 $2" = "-a 256" ] || exit 1\nexec %s "$3"\n' "$SHA256SUM" > "$W/shasum/shasum"
chmod 755 "$W/wget/wget" "$W/shasum/shasum"
# the full set; tests drop curl or sha256sum by using another PATH
cp "$FAKE"/* "$W/nocurl/"
rm "$W/nocurl/curl"

# ------------------------------------------------------------------ the fake GitHub
fakebin() {  # out version arch [runs]
  if [ "${4:-runs}" = runs ]; then
    printf '#!/bin/sh\n# fake %s %s for %s\n[ "${1:-}" = --version ] && echo "%s %s"\nexit 0\n' "$APP" "$2" "$3" "$APP" "$2" > "$1"
  else
    printf '#!/bin/sh\n# fake %s that does not run here\necho "Illegal instruction" >&2\nexit 132\n' "$APP" > "$1"
  fi
}

release() {  # web-dir version: the assets of release v<version>
  rl_d=$1/github.com/$REPO/releases/download/v$2
  mkdir -p "$rl_d"
  for a in $ARCHES; do fakebin "$rl_d/$APP-$2-linux-$a" "$2" "$a"; done
  cp "$INSTALL" "$rl_d/install.sh"
  (cd "$rl_d" && "$SHA256SUM" -- * > "$W/sums.tmp" && mv "$W/sums.tmp" SHA256SUMS)
}
release "$WEB" 1.1.0
release "$WEB" 1.2.3
mkdir -p "$WEB/github.com/$REPO/releases/latest/download"
cp "$WEB/github.com/$REPO/releases/download/v1.2.3/SHA256SUMS" "$WEB/github.com/$REPO/releases/latest/download/SHA256SUMS"

# the API: release JSON (pretty for latest, like GitHub; one line for the tag, keys in another order)
API=$WEB/api.github.com/repos/$REPO/releases
mkdir -p "$API/assets" "$API/tags"
api_release() {  # version first-id pretty|compact
  ar_v=$1
  ar_id=$2
  ar_d=$WEB/github.com/$REPO/releases/download/v$ar_v
  {
    printf '{\n  "url": "https://api.github.com/repos/%s/releases/9%s",\n' "$REPO" "$ar_id"
    printf '  "assets_url": "https://api.github.com/repos/%s/releases/9%s/assets",\n' "$REPO" "$ar_id"
    printf '  "upload_url": "https://uploads.github.com/repos/%s/releases/9%s/assets{?name,label}",\n' "$REPO" "$ar_id"
    printf '  "author": {\n    "login": "juricabi",\n    "id": 1,\n    "url": "https://api.github.com/users/juricabi"\n  },\n'
    printf '  "tag_name": "v%s",\n  "name": "v%s, \\"the\\" [new] {one}",\n  "assets": [\n' "$ar_v" "$ar_v"
    ar_first=1
    for ar_f in SHA256SUMS install.sh $(for a in $ARCHES; do echo "$APP-$ar_v-linux-$a"; done); do
      [ "$ar_first" = 1 ] || printf '    },\n'
      ar_first=0
      cp "$ar_d/$ar_f" "$API/assets/$ar_id"
      if [ "$3" = pretty ]; then
        printf '    {\n      "url": "https://api.github.com/repos/%s/releases/assets/%s",\n      "id": %s,\n' "$REPO" "$ar_id" "$ar_id"
        printf '      "name": "%s",\n      "uploader": {\n        "login": "github-actions[bot]",\n' "$ar_f"
        printf '        "url": "https://api.github.com/users/github-actions%%5Bbot%%5D"\n      },\n'
        printf '      "browser_download_url": "https://github.com/%s/releases/download/v%s/%s"\n' "$REPO" "$ar_v" "$ar_f"
      else
        printf '    {"browser_download_url":"https://github.com/%s/releases/download/v%s/%s","name":"%s",' "$REPO" "$ar_v" "$ar_f" "$ar_f"
        printf '"uploader":{"url":"https://api.github.com/users/x"},"url":"https://api.github.com/repos/%s/releases/assets/%s"\n' "$REPO" "$ar_id"
      fi
      ar_id=$((ar_id + 1))
    done
    printf '    }\n  ],\n  "body": "a,\\"url\\": \\"https://api.github.com/repos/x/y/releases/assets/999\\", [b] {c}"\n}\n'
  } > "$W/json.tmp"
  if [ "$3" = pretty ]; then cat "$W/json.tmp"; else tr -d '\n' < "$W/json.tmp"; fi
}
api_release 1.2.3 101 pretty > "$API/latest"
api_release 1.1.0 201 compact > "$API/tags/v1.1.0"
if command -v python3 >/dev/null 2>&1; then
  python3 -c 'import json, sys; [json.load(open(f)) for f in sys.argv[1:]]' "$API/latest" "$API/tags/v1.1.0" ||
    { echo "test_install.sh: the fake API answers are not valid JSON" >&2; exit 1; }
fi
TOKEN=tok-$$-secret

# ------------------------------------------------------------------ running the installer
# Settings for the next run (reset by begin): the fake machine, user and environment.
begin() {  # case name
  CASE=$1
  T_UID=0 T_USER=root T_MACHINE=x86_64 T_SUDO='' T_TOKEN='' T_PATH=$PATH_STD T_HOME=/root T_RT='' T_GROUPS=''
  T_NOINPUT=''
  rm -rf "${W:?}/root" "${W:?}/home"
  mkdir -p "$W/root"
  R=$W/root
}

inst() {  # options...
  # never a real system install: a run as (fake) root needs --root, --user or --dry-run
  ig_safe=0
  for ig_a in "$@"; do
    case $ig_a in --root|--root=*|--dry-run|-n|--user) ig_safe=1 ;; esac
  done
  if [ "$T_UID" = 0 ] && [ "$ig_safe" = 0 ]; then
    echo "test_install.sh: BUG in the test: a root run without --root, --user or --dry-run ($*)" >&2
    exit 2
  fi
  : > "$LOG"
  : > "$CURLCFG"
  rm -f "$OUT"
  # shellcheck disable=SC2086  # SH is "bash --posix" or "busybox sh"
  env -i PATH="$T_PATH" HOME="$T_HOME" TMPDIR="$W/tmp" FAKE_LOG="$LOG" FAKE_CURLCFG="$CURLCFG" FAKE_WEB="$WEB" \
    FAKE_TOKEN="$TOKEN" FAKE_UID="$T_UID" FAKE_USER="$T_USER" FAKE_MACHINE="$T_MACHINE" FAKE_BOB_GROUPS="$T_GROUPS" \
    ELRS_WIFI_JOYSTICK_REPO="$REPO" ${T_SUDO:+"SUDO_USER=$T_SUDO"} ${T_TOKEN:+"GITHUB_TOKEN=$T_TOKEN"} \
    ${T_RT:+"XDG_RUNTIME_DIR=$T_RT"} ${T_NOINPUT:+"FAKE_NO_INPUT=1"} \
    $SH "$INSTALL" "$@" > "$OUT" 2>&1
  RC=$?
}

files_in() { (cd "$1" && find . \( -type f -o -type l \) | LC_ALL=C sort); }

record_is() {  # record file, expected lines...
  ri_f=$1
  shift
  {
    echo "# $APP install.sh: what it installed (sh install.sh --uninstall removes it)"
    for ri_l in "$@"; do echo "$ri_l"; done
  } > "$W/want.rec"
  cmp -s "$ri_f" "$W/want.rec"
}

unit_for() {  # exec path -> the unit the installer should write
  sed "s|^ExecStart=/usr/local/bin/$APP |ExecStart=$1 |" "$PKG/$APP.service"
}

make_socket() {  # path: a unix socket file (a running systemd user manager), with python3
  command -v python3 >/dev/null 2>&1 || return 1
  mkdir -p "${1%/*}"
  python3 -c 'import socket, sys; socket.socket(socket.AF_UNIX).bind(sys.argv[1])' "$1"
}

BINREL=$WEB/github.com/$REPO/releases/download

# ================================================================== cases
case_options() {
  begin options
  T_UID=1000 T_USER=alice T_HOME=$W/home/alice
  inst --help
  check "--help exits 0" rc_is 0
  for o in --version --file --user --service --no-udev --uninstall --arch --root --dry-run GITHUB_TOKEN; do
    check "--help names $o" has "$o"
  done
  inst --frobnicate
  check "unknown option: exit 2" rc_is 2
  check "unknown option: said" has 'unknown option "--frobnicate"'
  for args in "--version" "--version=" "--version 1.0/../x" "--version latest!" "--arch mips" "--user=1" \
              "--file x --version 1.0.0" "--uninstall --service" "--uninstall --version 1.0.0" "--root"; do
    # shellcheck disable=SC2086  # the words of one case
    inst $args
    check "bad options \"$args\": exit 2" rc_is 2
  done
  inst --file "$W/nonexistent"
  check "--file that does not exist: exit 1" rc_is 1
  inst --root "$W/nonexistent"
  check "--root that does not exist: exit 1" rc_is 1
  T_UID=0 T_SUDO=bob
  inst --user
  check "--user under sudo: exit 2" rc_is 2
  check "--user under sudo: says to run it without sudo" has "run it as bob, without sudo"
  check "nothing ran" not_logged curl
}

case_arch() {
  begin arch
  for pair in x86_64:x86_64 amd64:x86_64 i386:i686 i486:i686 i586:i686 i686:i686 aarch64:aarch64 arm64:aarch64 \
              armv7l:armv7 armv8l:armv7 armv6l:armv6 riscv64:riscv64; do
    T_MACHINE=${pair%%:*}
    inst --dry-run --root "$R"
    check "uname -m $T_MACHINE: exit 0" rc_is 0
    check "uname -m $T_MACHINE -> ${pair#*:}" has "for ${pair#*:} from github.com/$REPO"
  done
  check "dry run: nothing written" empty_dir "$R"
  check "dry run: nothing downloaded" not_logged curl
  for m in armv5tel mips64 s390x ppc64le sparc64 aarch64_be; do
    T_MACHINE=$m
    inst --root "$R"
    check "uname -m $m: exit 1" rc_is 1
    check "uname -m $m: says there is no binary" has "has no ready-made binary"
  done
  T_MACHINE=mips64
  inst --root "$R" --arch armv6 --dry-run
  check "--arch overrides uname" has "for armv6 from"
}

case_system_latest() {
  begin system-latest
  T_MACHINE=aarch64
  inst --root "$R"
  check "exit 0" rc_is 0
  check "binary = the release's aarch64 binary" same "$R/usr/local/bin/$APP" "$BINREL/v1.2.3/$APP-1.2.3-linux-aarch64"
  check "binary mode 755" mode_is "$R/usr/local/bin/$APP" -rwxr-xr-x
  check "udev rule = packaging file" same "$R/etc/udev/rules.d/60-$APP.rules" "$PKG/60-$APP.rules"
  check "udev rule mode 644" mode_is "$R/etc/udev/rules.d/60-$APP.rules" -rw-r--r--
  check "modules-load = packaging file" same "$R/etc/modules-load.d/$APP-uinput.conf" "$PKG/$APP-uinput.conf"
  check "no unit without --service" gone "$R/etc/systemd/user/$APP.service"
  check "record" record_is "$R/usr/local/share/$APP/installed" "file /usr/local/bin/$APP" \
    "file /etc/udev/rules.d/60-$APP.rules" "file /etc/modules-load.d/$APP-uinput.conf"
  check "checksum checked" has "Checksum OK"
  check "version shown" has "Version: 1.2.3"
  check "--root: modprobe skipped" has "skipped (--root): modprobe uinput"
  check "--root: udevadm skipped" has "skipped (--root): udevadm control --reload"
  check "nothing loaded" not_logged modprobe
  check "udev not touched" not_logged udevadm
  check "no systemctl" not_logged systemctl
  check "public URL of the latest SHA256SUMS" logged "[https://github.com/$REPO/releases/latest/download/SHA256SUMS]"
  check "public URL of the binary" logged "[https://github.com/$REPO/releases/download/v1.2.3/$APP-1.2.3-linux-aarch64]"
  check "no token sent" empty_file "$CURLCFG"
  check "firewall hint" has "sudo ufw allow 11000/udp"
  check "web page hint" has "http://127.0.0.1:11000/"
  check "no temp files left in the root" no_temp "$R"
  check "temp dir cleaned" empty_dir "$W/tmp"
  check "only these files" files_are "$R" "./etc/modules-load.d/$APP-uinput.conf" "./etc/udev/rules.d/60-$APP.rules" \
    "./usr/local/bin/$APP" "./usr/local/share/$APP/installed"
}
empty_file() { [ ! -s "$1" ]; }
no_temp() { [ -z "$(find "$1" -name '.*.tmp.*')" ]; }
files_are() {
  fa_d=$1
  shift
  printf '%s\n' "$@" | LC_ALL=C sort > "$W/want.files"
  files_in "$fa_d" > "$W/got.files"
  cmp -s "$W/want.files" "$W/got.files"
}

case_versions_and_update() {
  begin versions
  T_MACHINE=armv7l
  inst --root "$R" --version 1.1.0
  check "--version 1.1.0: exit 0" rc_is 0
  check "--version 1.1.0: that binary" same "$R/usr/local/bin/$APP" "$BINREL/v1.1.0/$APP-1.1.0-linux-armv7"
  check "--version 1.1.0: its SHA256SUMS" logged "[https://github.com/$REPO/releases/download/v1.1.0/SHA256SUMS]"
  check "--version 1.1.0: not latest" not_logged "latest"
  inst --root "$R" --version v1.2.3
  check "--version v1.2.3 (with v): exit 0" rc_is 0
  check "update: replaced" same "$R/usr/local/bin/$APP" "$BINREL/v1.2.3/$APP-1.2.3-linux-armv7"
  check "update: says what it replaces" has "Replacing 1.1.0"
  check "update: the record is the same" record_is "$R/usr/local/share/$APP/installed" "file /usr/local/bin/$APP" \
    "file /etc/udev/rules.d/60-$APP.rules" "file /etc/modules-load.d/$APP-uinput.conf"
  inst --root "$R" --version 9.9.9
  check "a release that does not exist: exit 1" rc_is 1
  check "a release that does not exist: said" has "cannot download release v9.9.9"
  check "...and the binary stays" same "$R/usr/local/bin/$APP" "$BINREL/v1.2.3/$APP-1.2.3-linux-armv7"
  inst --root "$R" --no-udev --version latest
  check "--no-udev later: the rule stays on the record" record_is "$R/usr/local/share/$APP/installed" \
    "file /usr/local/bin/$APP" "file /etc/udev/rules.d/60-$APP.rules" "file /etc/modules-load.d/$APP-uinput.conf"
  rm -rf "${R:?}"/*
  inst --root "$R" --no-udev
  check "--no-udev: no rule" gone "$R/etc/udev/rules.d/60-$APP.rules"
  check "--no-udev: no modules-load file" gone "$R/etc/modules-load.d/$APP-uinput.conf"
  check "--no-udev: record" record_is "$R/usr/local/share/$APP/installed" "file /usr/local/bin/$APP"
}

case_bad_downloads() {
  begin bad-downloads
  bad=$W/badweb
  rm -rf "$bad"
  cp -R "$WEB" "$bad"
  printf 'x' >> "$bad/github.com/$REPO/releases/download/v1.2.3/$APP-1.2.3-linux-x86_64"
  WEB_SAVE=$WEB
  WEB=$bad
  inst --root "$R"
  check "checksum mismatch: exit 1" rc_is 1
  check "checksum mismatch: said" has "does not match its checksum"
  check "checksum mismatch: nothing installed" empty_dir "$R"
  check "checksum mismatch: temp dir cleaned" empty_dir "$W/tmp"
  # the latest release without this architecture
  grep -v -- "-linux-riscv64\$" "$bad/github.com/$REPO/releases/latest/download/SHA256SUMS" > "$W/s" &&
    mv "$W/s" "$bad/github.com/$REPO/releases/latest/download/SHA256SUMS"
  T_MACHINE=riscv64
  inst --root "$R"
  check "no binary for the arch: exit 1" rc_is 1
  check "no binary for the arch: said" has "has no binary for riscv64"
  # a checksum line that is not a checksum
  sed "s/^[0-9a-f]*  \\($APP-1.1.0-linux-riscv64\\)\$/zz  \\1/" "$bad/github.com/$REPO/releases/download/v1.1.0/SHA256SUMS" > "$W/s" &&
    mv "$W/s" "$bad/github.com/$REPO/releases/download/v1.1.0/SHA256SUMS"
  inst --root "$R" --version 1.1.0
  check "a broken SHA256SUMS line: exit 1" rc_is 1
  check "a broken SHA256SUMS line: said" has "SHA256SUMS has no checksum for"
  WEB=$WEB_SAVE
  # a binary that does not run on this machine (not --root): refused
  T_UID=1000 T_USER=alice T_HOME=$W/home/alice
  mkdir -p "$T_HOME"
  fakebin "$W/norun" 1.2.3 x86_64 norun
  inst --user --file "$W/norun"
  check "a binary that does not run here: exit 1" rc_is 1
  check "a binary that does not run here: said" has "does not run on this machine"
  check "a binary that does not run here: nothing installed" empty_dir "$T_HOME"
  # with --root it may be for another machine
  T_UID=0 T_USER=root T_HOME=/root
  inst --root "$R" --file "$W/norun"
  check "--root + a binary for another machine: exit 0" rc_is 0
  check "--root + a binary for another machine: installed" same "$R/usr/local/bin/$APP" "$W/norun"
  check "--root + a binary for another machine: noted" has "not run here"
}

case_token() {
  begin token
  T_TOKEN=$TOKEN
  T_MACHINE=riscv64
  inst --root "$R"
  check "token, latest: exit 0" rc_is 0
  check "token, latest: the binary" same "$R/usr/local/bin/$APP" "$BINREL/v1.2.3/$APP-1.2.3-linux-riscv64"
  check "token, latest: the API" logged "[https://api.github.com/repos/$REPO/releases/latest]"
  check "token, latest: assets by id" logged "[https://api.github.com/repos/$REPO/releases/assets/108]"
  check "token: never on a command line" not_logged "$TOKEN"
  check "token: given to curl on stdin" grep -qF "Authorization: Bearer $TOKEN" "$CURLCFG"
  check "token: not the decoy asset in the notes" not_logged "assets/999"
  T_MACHINE=i686
  inst --root "$R" --version 1.1.0
  check "token, a tag (one-line JSON): exit 0" rc_is 0
  check "token, a tag: the binary" same "$R/usr/local/bin/$APP" "$BINREL/v1.1.0/$APP-1.1.0-linux-i686"
  check "token, a tag: the tag's API" logged "[https://api.github.com/repos/$REPO/releases/tags/v1.1.0]"
  T_TOKEN=wrong
  inst --root "$R"
  check "a wrong token: exit 1" rc_is 1
  check "a wrong token: said" has "cannot read release latest"
  T_TOKEN=$TOKEN
  T_PATH=$PATH_WGET
  inst --root "$R"
  check "token without curl: exit 1" rc_is 1
  check "token without curl: said" has "needs curl"
}

case_wget_and_shasum() {
  begin wget-shasum
  T_PATH=$PATH_WGET
  inst --root "$R"
  check "wget: exit 0" rc_is 0
  check "wget: the binary" same "$R/usr/local/bin/$APP" "$BINREL/v1.2.3/$APP-1.2.3-linux-x86_64"
  check "wget: used" logged "wget [-q] [-O]"
  rm -rf "${R:?}"/*
  T_PATH=$PATH_NOSHA
  inst --root "$R"
  check "shasum: exit 0" rc_is 0
  check "shasum: used" logged "shasum -a 256"
  T_PATH=$PATH_NOTOOL
  inst --root "$R"
  check "no checksum tool: exit 1" rc_is 1
  check "no checksum tool: said" has "neither sha256sum nor shasum"
}

case_file() {
  begin file
  fakebin "$W/mybin" 7.0.0 x86_64
  chmod 644 "$W/mybin"             # e.g. a CI artifact: the exec bit is lost on the way
  inst --root "$R" --file "$W/mybin"
  check "--file: exit 0" rc_is 0
  check "--file: installed" same "$R/usr/local/bin/$APP" "$W/mybin"
  check "--file: executable" mode_is "$R/usr/local/bin/$APP" -rwxr-xr-x
  check "--file: version shown" has "Version: 7.0.0"
  check "--file: no download" not_logged curl
  check "--file: the source is untouched" mode_is "$W/mybin" -rw-r--r--
}

case_user() {
  begin user
  T_UID=1000 T_USER=alice T_HOME="$W/home/al ice"
  inst --root "$R"
  check "user (default without root): exit 0" rc_is 0
  check "user: says why" has "Not root: installing for alice only"
  H=$R$T_HOME
  check "user: the binary in ~/.local/bin" same "$H/.local/bin/$APP" "$BINREL/v1.2.3/$APP-1.2.3-linux-x86_64"
  check "user: no system files" files_are "$R" "./${T_HOME#/}/.local/bin/$APP" "./${T_HOME#/}/.local/share/$APP/installed"
  check "user: record" record_is "$H/.local/share/$APP/installed" "file $T_HOME/.local/bin/$APP"
  check "user: prints the udev rule to install with sudo" has "sudo tee /etc/udev/rules.d/60-$APP.rules"
  check "user: the printed rule is the packaged one" has "$(grep '^KERNEL==' "$PKG/60-$APP.rules")"
  inst --root "$R" --user
  check "--user: exit 0" rc_is 0
  check "--user: no 'not root' note" hasnt "Not root"
  inst --root "$R" --user --service
  check "user --service: exit 0" rc_is 0
  unit_for "%h/.local/bin/$APP" > "$W/want.unit"
  check "user --service: the unit (ExecStart in the home)" same "$H/.config/systemd/user/$APP.service" "$W/want.unit"
  check "user --service: record" record_is "$H/.local/share/$APP/installed" "file $T_HOME/.local/bin/$APP" \
    "file $T_HOME/.config/systemd/user/$APP.service" "service alice"
  check "user --service: enabling skipped (--root)" \
    has "skipped (--root): as alice: systemctl --user daemon-reload && systemctl --user enable $APP.service"
  check "user --service: no systemctl" not_logged systemctl
  # real root without sudo: --user installs into root's own home
  T_UID=0 T_USER=root T_HOME=/root
  inst --root "$R" --user --no-udev
  check "root --user: exit 0" rc_is 0
  check "root --user: into /root" same "$R/root/.local/bin/$APP" "$BINREL/v1.2.3/$APP-1.2.3-linux-x86_64"
}

case_system_service() {
  begin system-service
  T_SUDO=bob
  inst --root "$R" --service
  check "system --service: exit 0" rc_is 0
  check "system --service: the packaged unit" same "$R/etc/systemd/user/$APP.service" "$PKG/$APP.service"
  check "system --service: record" record_is "$R/usr/local/share/$APP/installed" "file /usr/local/bin/$APP" \
    "file /etc/udev/rules.d/60-$APP.rules" "file /etc/modules-load.d/$APP-uinput.conf" \
    "file /etc/systemd/user/$APP.service" "service bob"
  check "system --service: enabling for bob skipped (--root)" has "skipped (--root): as bob: systemctl --user daemon-reload"
  check "system --service: linger hint" has "sudo loginctl enable-linger bob"
  T_SUDO=
  inst --root "$R"
  check "update without --service: exit 0" rc_is 0
  check "update: the service is restarted (if running)" has "as bob: systemctl --user daemon-reload && systemctl --user try-restart"
  check "update: the record keeps the service" grep -qx "service bob" "$R/usr/local/share/$APP/installed"
  rm -rf "${R:?}"/*
  inst --root "$R" --service
  check "--service as root without sudo: exit 1" rc_is 1
  check "--service as root without sudo: said" has "--service makes a systemd user service"
  check "--service as root without sudo: nothing installed" empty_dir "$R"
}

case_uninstall() {
  begin uninstall
  T_SUDO=bob
  inst --root "$R" --service
  # things that are not ours, and the link "systemctl --user enable" makes
  mkdir -p "$R/home/bob/.config/$APP" "$R/home/bob/.config/systemd/user/default.target.wants" "$R/etc"
  echo "channels = 8" > "$R/home/bob/.config/$APP/config"
  echo "# not ours" > "$R/etc/udev/rules.d/70-other.rules"
  echo "root:x:0:0::/root:/bin/sh" > "$R/etc/passwd"
  ln -s "/etc/systemd/user/$APP.service" "$R/home/bob/.config/systemd/user/default.target.wants/$APP.service"
  echo "file /etc/passwd" >> "$R/usr/local/share/$APP/installed"
  echo "service bob;rm -rf /" >> "$R/usr/local/share/$APP/installed"
  T_SUDO=
  files_in "$R" > "$W/before"
  inst --root "$R" --uninstall --dry-run
  check "dry-run uninstall: exit 0" rc_is 0
  files_in "$R" > "$W/after"
  check "dry-run uninstall: nothing removed" same "$W/before" "$W/after"
  check "dry-run uninstall: says what it would remove" has "would remove $R/usr/local/bin/$APP"
  inst --root "$R" --uninstall
  check "uninstall: exit 0" rc_is 0
  check "uninstall: the edited record does not remove more" has 'ignoring "file /etc/passwd"'
  check "uninstall: a bad service line is ignored" has 'ignoring "service bob;rm -rf /"'
  check "uninstall: only what it installed is gone" files_are "$R" "./etc/passwd" "./etc/udev/rules.d/70-other.rules" \
    "./home/bob/.config/$APP/config"
  check "uninstall: the folders stay" test -d "$R/usr/local/bin"
  check "uninstall: its record folder is gone" gone "$R/usr/local/share/$APP"
  check "uninstall: disabling skipped (--root)" has "skipped (--root): as bob: systemctl --user disable --now $APP.service"
  check "uninstall: settings note" has "Your settings stay in ~/.config/$APP/"
  inst --root "$R" --uninstall
  check "uninstall again: exit 0" rc_is 0
  check "uninstall again: nothing to remove" has "Nothing to remove"
  # installed for the user, uninstalled with sudo: a hint
  T_UID=1000 T_USER=alice T_HOME=/home/alice
  inst --root "$R" --no-udev
  T_UID=0 T_USER=root T_HOME=/root T_SUDO=alice
  inst --root "$R" --uninstall
  check "uninstall with sudo of a user install: hint" has "If alice installed it without root"
  check "...and the user's install stays" test -x "$R/home/alice/.local/bin/$APP"
  T_UID=1000 T_USER=alice T_HOME=/home/alice T_SUDO=''
  inst --root "$R" --uninstall
  check "user uninstall: exit 0" rc_is 0
  check "user uninstall: gone" gone "$R/home/alice/.local/bin/$APP"
  check "user uninstall: record gone" gone "$R/home/alice/.local/share/$APP"
}

# without --root: a user install into a temporary HOME runs the user-level commands (fake systemctl)
case_live_user() {
  begin live-user
  T_UID=1000 T_USER=alice T_HOME=$W/home/alice T_RT=$W/home/rt
  mkdir -p "$T_HOME" "$T_RT"
  fakebin "$W/mybin" 7.0.0 x86_64
  inst --user --file "$W/mybin" --service
  check "live, no session: exit 0" rc_is 0
  check "live: installed" same "$T_HOME/.local/bin/$APP" "$W/mybin"
  check "live: PATH note" has "$T_HOME/.local/bin is not on your PATH"
  check "live, no session: enabled by the link" test -L "$T_HOME/.config/systemd/user/default.target.wants/$APP.service"
  check "live, no session: the link points at the unit" \
    test "$(readlink "$T_HOME/.config/systemd/user/default.target.wants/$APP.service")" = "$T_HOME/.config/systemd/user/$APP.service"
  check "live, no session: said" has "it starts when alice logs in"
  check "live, no session: no systemctl" not_logged "systemctl --user"
  inst --user --uninstall
  check "live uninstall, no session: exit 0" rc_is 0
  check "live uninstall, no session: the link is gone" gone "$T_HOME/.config/systemd/user/default.target.wants/$APP.service"
  check "live uninstall, no session: the unit is gone" gone "$T_HOME/.config/systemd/user/$APP.service"
  check "live uninstall: the binary is gone" gone "$T_HOME/.local/bin/$APP"
  if make_socket "$T_RT/systemd/private"; then
    inst --user --file "$W/mybin" --service
    check "live, session: exit 0" rc_is 0
    check "live, session: daemon-reload" logged "systemctl --user daemon-reload (XDG_RUNTIME_DIR=$T_RT)"
    check "live, session: enable" logged "systemctl --user enable $APP.service"
    check "live, session: (re)start" logged "systemctl --user restart $APP.service"
    check "live, session: said" has "Service enabled and started for alice: active"
    inst --user --file "$W/mybin"
    check "live update: try-restart" logged "systemctl --user try-restart $APP.service"
    inst --user --uninstall
    check "live uninstall, session: disable --now" logged "systemctl --user disable --now $APP.service"
    check "live uninstall, session: daemon-reload after" logged "systemctl --user daemon-reload"
    check "live uninstall, session: unit gone" gone "$T_HOME/.config/systemd/user/$APP.service"
  else
    echo "  (python3 is missing: the live session case is skipped)"
  fi
}

# ================================================================== run
if [ -n "${SHELLS:-}" ]; then
  LIST=$SHELLS
else
  LIST=
  for s in sh dash bash busybox; do
    sp=$(ext "$s") || continue
    case $s in
      bash) LIST="$LIST
$sp --posix" ;;
      busybox) "$sp" sh -c : 2>/dev/null && LIST="$LIST
$sp sh" ;;
      *) LIST="$LIST
$sp" ;;
    esac
  done
fi
# A shell that runs its own id/uname (busybox built with "standalone" applets) would bypass the fakes
# and see the real user, maybe root: it only gets a smoke test that cannot change anything.
uses_fakes() {
  # shellcheck disable=SC2086  # SH is "bash --posix" or "busybox sh"
  uf_out=$(env -i PATH="$PATH_STD" FAKE_UID=4242 FAKE_USER=vet FAKE_MACHINE=vet-machine FAKE_LOG=/dev/null \
    $SH -c 'id -u; id -un; uname -m' 2>/dev/null)
  [ "$uf_out" = "4242
vet
vet-machine" ]
}
case_smoke() {
  begin smoke
  T_UID=1000 T_USER=alice T_HOME=$W/home/alice
  inst --help
  check "--help exits 0" rc_is 0
  inst --dry-run --root "$R" --arch aarch64
  check "dry run exits 0" rc_is 0
  check "dry run: the plan" has "would download $APP (the latest release) for aarch64"
  check "dry run: nothing written" empty_dir "$R"
}
suite() {
  for c in case_options case_arch case_system_latest case_versions_and_update case_bad_downloads case_token \
           case_wget_and_shasum case_file case_user case_system_service case_uninstall case_live_user; do
    $c
  done
}
NSH=0
FIRST=
while IFS= read -r SH; do
  [ -n "$SH" ] || continue
  SHNAME=$SH
  NSH=$((NSH + 1))
  if ! uses_fakes; then
    echo "== install.sh under $SH: smoke test only (it runs its own id/uname, not the fakes on PATH)"
    case_smoke
    continue
  fi
  echo "== install.sh under $SH"
  suite
  [ -n "$FIRST" ] || FIRST=$SH
done <<EOF
$LIST
EOF
[ "$NSH" -gt 0 ] || { echo "test_install.sh: no shell to test with" >&2; exit 1; }
# busybox's sed, awk, grep, tr, mktemp, sha256sum... (Alpine, OpenWrt): BUSYBOX=path, or one on PATH
BB=${BUSYBOX:-}
[ -n "$BB" ] || BB=$(ext busybox) || BB=
if [ -n "$BB" ] && [ -n "$FIRST" ]; then
  make_sys "$W/sysbb" "$BB"
  use_sys "$W/sysbb"
  SH=$FIRST
  SHNAME="$FIRST, busybox tools"
  echo "== install.sh under $SHNAME ($BB)"
  suite
elif [ -z "$BB" ]; then
  echo "== (no busybox here: the busybox tools pass is skipped; BUSYBOX=/path/to/busybox to run it)"
fi
if [ "$NFAIL" = 0 ]; then
  echo "test_install.sh: $NCHECK checks passed ($NSH shells)"
  exit 0
fi
echo "test_install.sh: $NFAIL of $NCHECK checks FAILED"
exit 1
