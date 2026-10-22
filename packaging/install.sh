#!/bin/sh
# Install, update or remove elrs-wifi-joystick: the program, a udev rule that gives access to
# /dev/uinput (the virtual joystick), the uinput module at boot, and optionally a systemd user service.
# https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Linux - MIT License.
#
#   curl -fsSLO https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Linux/releases/latest/download/install.sh
#   sudo sh install.sh            (or: sh install.sh --help)
#
# POSIX sh (dash, bash, busybox ash), standalone: it needs nothing next to it. Run it again to update.
# It keeps a list of what it installed (<prefix>/share/elrs-wifi-joystick/installed), and --uninstall
# removes exactly what is on that list.
set -eu
umask 022

REPO=${ELRS_WIFI_JOYSTICK_REPO:-juricabi/ELRS-Crossfire-WiFi-Joystick-Linux}
APP=elrs-wifi-joystick
RULE_NAME=60-elrs-wifi-joystick.rules
MODLOAD_NAME=elrs-wifi-joystick-uinput.conf
UNIT_NAME=elrs-wifi-joystick.service
ARCHES="x86_64 i686 aarch64 armv7 armv6 riscv64"

# packaging/60-elrs-wifi-joystick.rules, packaging/elrs-wifi-joystick-uinput.conf and
# packaging/elrs-wifi-joystick.service, byte for byte (tests/test_install.sh checks it): this script
# also works on its own, downloaded alone or piped into sh.
RULE_TEXT='# elrs-wifi-joystick: write access to /dev/uinput, which it needs to create its virtual joystick.
#   uaccess      the user logged in at this machine'"'"'s screen gets access at once (an ACL; desktops)
#   GROUP, MODE  members of the "input" group get it too (SSH logins, a service started at boot)
#   static_node  the same permissions for /dev/uinput before the uinput module is loaded
# Access to /dev/uinput lets that user'"'"'s programs create input devices (joysticks, also keyboards).
# Installed by packaging/install.sh: https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Linux
KERNEL=="uinput", SUBSYSTEM=="misc", TAG+="uaccess", GROUP="input", MODE="0660", OPTIONS+="static_node=uinput"'

MODLOAD_TEXT='# Load the uinput kernel module at boot: elrs-wifi-joystick creates its virtual joystick with it.
# Installed by packaging/install.sh: https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Linux
uinput'

# shellcheck disable=SC2016  # $USER is for the reader of the unit file
UNIT_TEXT='# elrs-wifi-joystick as a systemd user service: it runs while you are logged in (and from boot with
# "sudo loginctl enable-linger $USER"). Installed by packaging/install.sh --service.
#   systemctl --user status elrs-wifi-joystick       state and the last log lines
#   systemctl --user stop|start|restart elrs-wifi-joystick
#   journalctl --user -u elrs-wifi-joystick -f       the log
# Settings: the web page (http://127.0.0.1:11000/) or ~/.config/elrs-wifi-joystick/config.
# https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Linux

[Unit]
Description=ELRS / Crossfire WiFi joystick
Documentation=https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Linux

[Service]
Type=simple
ExecStart=/usr/local/bin/elrs-wifi-joystick --daemon
Restart=on-failure
RestartSec=5
# 2 = bad options, 3 = already running (started by hand): starting again would not help
RestartPreventExitStatus=2 3
NoNewPrivileges=yes

[Install]
WantedBy=default.target'

usage() {
  cat <<EOF
Usage: sh install.sh [options]

Installs elrs-wifi-joystick from its GitHub release (or a file you have), checked against the
release's SHA256SUMS. As root (sudo): the program in /usr/local/bin, a udev rule that lets the
logged-in user (and the "input" group) use /dev/uinput, and uinput loaded at every boot. Without
root: the program in ~/.local/bin only; it prints the commands that need root. Run it again to update.

  --version X      install release X (e.g. 1.0.0; default: the latest)
  --file PATH      install this binary instead of downloading one
  --user           install for this user only, in ~/.local/bin (the default without root)
  --service        also a systemd user service that runs the program (as the user who ran sudo,
                   or as you without root); enabled and started when that user has a session
  --no-udev        leave /dev/uinput access and the uinput module alone (remembered for updates)
  --udev           set them up again after an earlier --no-udev
  --uninstall      remove what this script installed (the settings in ~/.config stay)
  --arch A         the binary for A instead of this machine's: $ARCHES
  --root DIR       install into DIR instead of / (images, tests): nothing is loaded or started
  -n, --dry-run    only print what would be done
  -h, --help       this help

Environment: GITHUB_TOKEN (optional, only for a private fork: a token that can read it; needs curl;
when it cannot read the release, the public download is tried), ELRS_WIFI_JOYSTICK_REPO (owner/name,
default $REPO).
EOF
}

NL='
'                           # a newline
say() { printf '%s\n' "$*"; }
warn() { printf 'WARNING: %s\n' "$*" >&2; }
die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }
bad() { printf 'install.sh: %s - see --help\n' "$*" >&2; exit 2; }

# ------------------------------------------------------------------ options
WANT=
WANT_SET=0
FILE=
USER_MODE=0
SERVICE=0
UDEV=1
UDEV_SET=0
UNINSTALL=0
ROOT=
ROOT_SET=0
ARCH_OPT=
DRY=0
while [ $# -gt 0 ]; do
  opt=$1
  case $opt in
    --*=*) val=${opt#*=}; opt=${opt%%=*}; has_val=1 ;;
    *) val=; has_val=0 ;;
  esac
  case $opt in
    --version|--file|--root|--arch)
      if [ "$has_val" = 0 ]; then
        [ $# -ge 2 ] || bad "$opt needs a value"
        val=$2
        shift
      fi
      case $opt in
        --version) WANT=$val; WANT_SET=1 ;;
        --file) FILE=$val; [ -n "$FILE" ] || bad "--file needs a file name" ;;
        --root) ROOT=$val; ROOT_SET=1 ;;
        --arch) ARCH_OPT=$val ;;
      esac
      ;;
    --user|--service|--no-udev|--udev|--uninstall|--dry-run|--help)
      [ "$has_val" = 0 ] || bad "$opt takes no value"
      case $opt in
        --user) USER_MODE=1 ;;
        --service) SERVICE=1 ;;
        --no-udev) UDEV=0; UDEV_SET=1 ;;
        --udev) UDEV=1; UDEV_SET=1 ;;
        --uninstall) UNINSTALL=1 ;;
        --dry-run) DRY=1 ;;
        --help) usage; exit 0 ;;
      esac
      ;;
    -n) DRY=1 ;;
    -h) usage; exit 0 ;;
    *) bad "unknown option \"$opt\"" ;;
  esac
  shift
done

if [ "$WANT_SET" = 1 ]; then
  case $WANT in
    latest) WANT= ;;
    v[0-9]*|[0-9]*)
      WANT=${WANT#v}
      case $WANT in *[!0-9A-Za-z.+-]*) bad "--version: \"$WANT\" is not a version (e.g. 1.0.0)" ;; esac
      ;;
    *) bad "--version: \"$WANT\" is not a version (e.g. 1.0.0)" ;;
  esac
fi
if [ -n "$ARCH_OPT" ]; then
  case " $ARCHES " in *" $ARCH_OPT "*) ;; *) bad "--arch: \"$ARCH_OPT\" is not one of: $ARCHES" ;; esac
fi
if [ "$UNINSTALL" = 1 ]; then
  if [ -n "$FILE" ] || [ "$WANT_SET" = 1 ] || [ "$SERVICE" = 1 ] || [ -n "$ARCH_OPT" ]; then
    bad "--uninstall goes only with --user, --root and --dry-run"
  fi
fi
[ -z "$FILE" ] || [ "$WANT_SET" = 0 ] || bad "--file and --version: give one of them"
[ -z "$FILE" ] || [ -z "$ARCH_OPT" ] || bad "--file and --arch: give one of them"
if [ -n "$FILE" ]; then
  [ -f "$FILE" ] || die "--file: $FILE is not a file"
  [ -r "$FILE" ] || die "--file: cannot read $FILE"
fi
if [ "$ROOT_SET" = 1 ]; then
  [ -n "$ROOT" ] || bad "--root needs a directory"
  [ -d "$ROOT" ] || die "--root: $ROOT is not a directory"
  ROOT=$(cd "$ROOT" && pwd -P) || die "--root: cannot use $ROOT"
  [ "$ROOT" != / ] || ROOT=
fi

# ------------------------------------------------------------------ who, where
CUR_UID=$(id -u)
CUR_USER=$(id -un 2>/dev/null) || CUR_USER=
[ -n "$CUR_USER" ] || CUR_USER="#$CUR_UID"
INVOKER=${SUDO_USER:-${DOAS_USER:-}}
[ "$INVOKER" != root ] || INVOKER=

NOT_ROOT_NOTE=0
if [ "$USER_MODE" = 1 ]; then
  if [ "$CUR_UID" = 0 ] && [ -n "$INVOKER" ]; then
    bad "--user installs for one user and needs no root: run it as $INVOKER, without sudo"
  fi
  MODE=user
elif [ "$CUR_UID" = 0 ]; then
  MODE=system
else
  MODE=user
  NOT_ROOT_NOTE=1
fi

# Every path below is the one on the target system; files are written at "$ROOT$path".
if [ "$MODE" = system ]; then
  BIN_DIR=/usr/local/bin
  REC_DIR=/usr/local/share/$APP
  UNIT_DIR=/etc/systemd/user
  SVC_USER=$INVOKER
else
  case ${HOME:-} in
    /*) ;;
    *) die "HOME is not set: cannot find this user's home directory" ;;
  esac
  HOME=${HOME%/}
  BIN_DIR=$HOME/.local/bin
  REC_DIR=$HOME/.local/share/$APP
  # where the systemd user manager looks by default ($XDG_CONFIG_HOME may differ in this shell only)
  UNIT_DIR=$HOME/.config/systemd/user
  SVC_USER=$CUR_USER
fi
BIN=$BIN_DIR/$APP
REC=$REC_DIR/installed
RULE=/etc/udev/rules.d/$RULE_NAME
MODLOAD=/etc/modules-load.d/$MODLOAD_NAME
UNIT=$UNIT_DIR/$UNIT_NAME
if [ "$MODE" = system ]; then
  EXEC_PATH=$BIN
else
  EXEC_PATH=%h/.local/bin/$APP      # systemd's %h: the user's home, whatever its name
fi

# commands that change this machine (not just files): never with --root or --dry-run
LIVE=1
[ -z "$ROOT" ] || LIVE=0
[ "$DRY" = 0 ] || LIVE=0

TMPD=
PENDING=
# shellcheck disable=SC2329  # called by the EXIT trap
cleanup() {
  [ -z "$PENDING" ] || rm -f "$PENDING"
  [ -z "$TMPD" ] || rm -rf "$TMPD"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# ------------------------------------------------------------------ helpers
find_cmd() {  # a system tool, also when sbin is not on PATH (su without -)
  if command -v "$1" >/dev/null 2>&1; then command -v "$1"; return 0; fi
  for fc_d in /usr/sbin /sbin /usr/bin /bin; do
    if [ -x "$fc_d/$1" ]; then printf '%s\n' "$fc_d/$1"; return 0; fi
  done
  return 1
}

# say what a skipped command would have been
skipped() {
  if [ "$DRY" = 1 ]; then say "  would run: $*"; else say "  skipped (--root): $*"; fi
}

mkdirs() {  # logical dir
  if [ "$DRY" = 1 ]; then return 0; fi
  mkdir -p "$ROOT$1" || die "cannot create $ROOT$1"
}

# the data of a new file on disk before the rename that makes it the real one (a power cut right after
# an install must not leave an empty program or rule); sync with a file is coreutils 8.24+ / busybox
# 1.28+, a plain sync anywhere
sync_file() {  # path
  sync -- "$1" 2>/dev/null || sync 2>/dev/null || :
}

# write text to a file: temp file next to it, synced, then rename (nobody ever sees half a file)
put_text() {  # logical path, mode, text
  if [ "$DRY" = 1 ]; then say "  would write $ROOT$1"; return 0; fi
  mkdirs "${1%/*}"
  PENDING=$ROOT${1%/*}/.${1##*/}.tmp.$$
  printf '%s\n' "$3" > "$PENDING" || die "cannot write $ROOT$1"
  chmod "$2" "$PENDING" || die "cannot set the mode of $ROOT$1"
  sync_file "$PENDING"
  mv -f "$PENDING" "$ROOT$1" || die "cannot write $ROOT$1"
  PENDING=
}

put_file() {  # source, logical path, mode
  if [ "$DRY" = 1 ]; then say "  would install $ROOT$2"; return 0; fi
  mkdirs "${2%/*}"
  PENDING=$ROOT${2%/*}/.${2##*/}.tmp.$$
  cp "$1" "$PENDING" || die "cannot write $ROOT$2"
  chmod "$3" "$PENDING" || die "cannot set the mode of $ROOT$2"
  sync_file "$PENDING"
  mv -f "$PENDING" "$ROOT$2" || die "cannot write $ROOT$2"
  PENDING=
}

remove() {  # logical path
  if [ ! -e "$ROOT$1" ] && [ ! -L "$ROOT$1" ]; then say "  (already gone) $ROOT$1"; return 0; fi
  if [ "$DRY" = 1 ]; then say "  would remove $ROOT$1"; return 0; fi
  rm -f "$ROOT$1" || die "cannot remove $ROOT$1"
  say "  removed $ROOT$1"
}

user_home() {  # user -> home directory
  if command -v getent >/dev/null 2>&1; then
    getent passwd "$1" | cut -d: -f6
  else
    awk -F: -v u="$1" '$1 == u { print $6; exit }' /etc/passwd
  fi
}

in_group() {  # user group
  id -nG "$1" 2>/dev/null | tr ' ' '\n' | grep -qx "$2"
}

have_group() {
  if command -v getent >/dev/null 2>&1; then getent group "$1" >/dev/null 2>&1; else grep -q "^$1:" /etc/group; fi
}

as_user() {  # user command...: run a command as that user (from root)
  au_u=$1
  shift
  if [ "$au_u" = "$CUR_USER" ]; then
    "$@"
  elif command -v runuser >/dev/null 2>&1; then
    runuser -u "$au_u" -- "$@"
  elif command -v sudo >/dev/null 2>&1; then
    sudo -u "$au_u" -- "$@"
  else
    warn "cannot run a command as $au_u (there is neither runuser nor sudo)"
    return 1
  fi
}

user_rt() {  # user -> XDG_RUNTIME_DIR of that user
  if [ "$1" = "$CUR_USER" ] && [ -n "${XDG_RUNTIME_DIR:-}" ]; then
    printf '%s\n' "$XDG_RUNTIME_DIR"
  else
    ur_id=$(id -u "$1" 2>/dev/null) || return 1
    printf '/run/user/%s\n' "$ur_id"
  fi
}

has_session() {  # user: a systemd user manager runs for them
  command -v systemctl >/dev/null 2>&1 || return 1
  hs_rt=$(user_rt "$1") || return 1
  [ -S "$hs_rt/systemd/private" ] || [ -S "$hs_rt/bus" ]
}

user_systemctl() {  # user args...
  us_u=$1
  shift
  us_rt=$(user_rt "$us_u") || return 1
  as_user "$us_u" env XDG_RUNTIME_DIR="$us_rt" DBUS_SESSION_BUS_ADDRESS="unix:path=$us_rt/bus" systemctl --user "$@"
}

# the install record: "file PATH" and "service USER" lines
REC_LINES=
rec_add() {
  case "
$REC_LINES
" in
    *"
$1
"*) ;;
    *) REC_LINES="$REC_LINES
$1" ;;
  esac
}

# only these may be on the record of this kind of install (an edited record never removes more)
known_file() {
  case $1 in
    "$BIN"|"$UNIT") return 0 ;;
    "$RULE"|"$MODLOAD") [ "$MODE" = system ] ;;
    *) return 1 ;;
  esac
}

read_record() {  # -> OLD_FILES, OLD_SERVICES (newline-separated), OLD_NOUDEV; unknown lines are ignored
  OLD_FILES=
  OLD_SERVICES=
  OLD_NOUDEV=0
  [ -f "$ROOT$REC" ] || return 0
  while IFS= read -r rr_line || [ -n "$rr_line" ]; do
    case $rr_line in
      no-udev) OLD_NOUDEV=1 ;;
      "file "*)
        rr_p=${rr_line#file }
        if known_file "$rr_p"; then OLD_FILES="$OLD_FILES
$rr_p"; else warn "$ROOT$REC: ignoring \"$rr_line\" (not a file this script installs)"; fi
        ;;
      "service "*)
        rr_u=${rr_line#service }
        case $rr_u in
          ''|*[!A-Za-z0-9._-]*) warn "$ROOT$REC: ignoring \"$rr_line\"" ;;
          *) if [ "$MODE" = user ] && [ "$rr_u" != "$CUR_USER" ]; then warn "$ROOT$REC: ignoring \"$rr_line\""
             else OLD_SERVICES="$OLD_SERVICES
$rr_u"; fi ;;
        esac
        ;;
    esac
  done < "$ROOT$REC"
}

wants_link() {  # user -> the symlink "systemctl --user enable" makes
  if [ "$MODE" = user ]; then
    printf '%s\n' "$UNIT_DIR/default.target.wants/$UNIT_NAME"
  else
    wl_h=$(user_home "$1")
    [ -n "$wl_h" ] || return 1
    printf '%s\n' "${wl_h%/}/.config/systemd/user/default.target.wants/$UNIT_NAME"
  fi
}

uname_arch() {
  ua_m=$(uname -m)
  case $ua_m in
    x86_64|amd64) echo x86_64 ;;
    i686) echo i686 ;;   # i386..i586: a CPU before the Pentium Pro (no CMOV), which the i686 binary needs
    aarch64|arm64) echo aarch64 ;;
    armv7l|armv7|armv7hl|armv8l) echo armv7 ;;   # armv8l: a 32-bit program on a 64-bit ARM CPU
    armv6l|armv6) echo armv6 ;;
    riscv64) echo riscv64 ;;
    *) return 1 ;;
  esac
}

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | cut -d' ' -f1
  else
    die "neither sha256sum nor shasum is here: cannot check the download"
  fi
}

DL=
pick_downloader() {
  if command -v curl >/dev/null 2>&1; then DL=curl
  elif command -v wget >/dev/null 2>&1; then
    DL=wget
    [ -z "${GITHUB_TOKEN:-}" ] || warn "GITHUB_TOKEN is used only with curl (there is none here): trying the public download"
  else die "neither curl nor wget is here: install one, or use --file"
  fi
}

# fetch URL OUT [api|asset]: api/asset = GitHub API with the token; the token goes to curl on stdin,
# never on its command line (other users can read that), and curl sends it only to api.github.com
# (not to the host a download redirects to)
fetch() {
  f_url=$1
  f_out=$2
  f_kind=${3:-}
  if [ "$DL" = wget ]; then
    wget -q -O "$f_out" "$f_url"
    return
  fi
  set -- -fsSL --proto =https --retry 3 --connect-timeout 20 -o "$f_out"
  case $f_kind in
    api) set -- "$@" -H "Accept: application/vnd.github+json" -H "X-GitHub-Api-Version: 2022-11-28" ;;
    asset) set -- "$@" -H "Accept: application/octet-stream" ;;
  esac
  if [ -n "$f_kind" ] && [ -n "${GITHUB_TOKEN:-}" ]; then
    printf 'header = "Authorization: Bearer %s"\n' "$GITHUB_TOKEN" | curl -K - "$@" "$f_url"
  else
    curl "$@" "$f_url"
  fi
}

# ------------------------------------------------------------------ uninstall
if [ "$UNINSTALL" = 1 ]; then
  say "elrs-wifi-joystick: uninstalling ($MODE install${ROOT:+ under $ROOT})"
  if [ ! -f "$ROOT$REC" ]; then
    say "Nothing to remove: there is no record of an installation at $ROOT$REC."
    if [ "$MODE" = user ] && [ -f "$ROOT/usr/local/share/$APP/installed" ]; then
      say "It was installed with root: sudo sh install.sh --uninstall"
    elif [ "$MODE" = system ] && [ -n "$INVOKER" ]; then
      say "If $INVOKER installed it without root: sh install.sh --uninstall (as $INVOKER, without sudo)"
    fi
    exit 0
  fi
  read_record
  # the services first, while the program is still there
  for u in $OLD_SERVICES; do
    if [ "$LIVE" = 1 ] && has_session "$u"; then
      user_systemctl "$u" disable --now "$UNIT_NAME" || warn "could not stop the service of $u"
    elif [ "$LIVE" = 0 ]; then
      skipped "as $u: systemctl --user disable --now $UNIT_NAME"
    fi
    if wl=$(wants_link "$u"); then
      if [ -L "$ROOT$wl" ]; then
        if [ "$DRY" = 1 ]; then say "  would remove $ROOT$wl"
        elif [ -n "$ROOT" ] || [ "$MODE" = user ]; then rm -f "$ROOT$wl" && say "  removed $ROOT$wl"
        else as_user "$u" rm -f "$wl" && say "  removed $wl"
        fi
      fi
    fi
  done
  rule_gone=0
  unit_gone=0
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    remove "$f"
    [ "$f" != "$RULE" ] || rule_gone=1
    [ "$f" != "$UNIT" ] || unit_gone=1
  done <<EOF
$OLD_FILES
EOF
  if [ "$unit_gone" = 1 ]; then
    for u in $OLD_SERVICES; do
      if [ "$LIVE" = 1 ]; then
        if has_session "$u"; then user_systemctl "$u" daemon-reload || true; fi
      else
        skipped "as $u: systemctl --user daemon-reload"
      fi
    done
  fi
  if [ "$rule_gone" = 1 ]; then
    if [ "$LIVE" = 1 ]; then
      if UDEVADM=$(find_cmd udevadm); then
        "$UDEVADM" control --reload 2>/dev/null &&
          "$UDEVADM" trigger --action=change --subsystem-match=misc --sysname-match=uinput 2>/dev/null || true
      fi
    else
      skipped "udevadm control --reload && udevadm trigger --action=change --subsystem-match=misc --sysname-match=uinput"
    fi
  fi
  if [ "$DRY" = 1 ]; then
    say "  would remove $ROOT$REC"
  else
    rm -f "$ROOT$REC"
    rmdir "$ROOT$REC_DIR" 2>/dev/null || true
  fi
  say "Done. Your settings stay in ~/.config/$APP/ (delete that folder to forget them)."
  [ "$rule_gone" = 0 ] || say "/dev/uinput keeps the access it has now until the next reboot."
  exit 0
fi

# ------------------------------------------------------------------ install
if [ "$SERVICE" = 1 ] && [ -z "$SVC_USER" ]; then
  die "--service makes a systemd user service, and root has no user to run it as: run this with sudo from \
the account that should run it, or as that user without sudo (installs in ~/.local/bin)"
fi
# a system without systemd (OpenRC, runit, sysvinit: Alpine, Void, Devuan, Gentoo, ...) would never start the
# unit: say so instead of claiming it starts at login (with --root it is for another system's image)
if [ "$SERVICE" = 1 ] && [ "$LIVE" = 1 ] && ! command -v systemctl >/dev/null 2>&1; then
  die "--service makes a systemd user service, and this system has no systemd (no systemctl): start \
\"$APP --daemon\" from your init system or your session's autostart instead - nothing was installed"
fi
ARCH=$ARCH_OPT
if [ -z "$FILE" ]; then
  if [ -z "$ARCH" ]; then
    ARCH=$(uname_arch) || die "this machine ($(uname -m)) has no ready-made binary (there are: $ARCHES) - build it \
from source: https://github.com/$REPO/blob/HEAD/docs/INSTALL.md#building-from-source"
  fi
fi

say "elrs-wifi-joystick installer: $MODE install into $ROOT$BIN_DIR${ARCH:+ ($ARCH)}"
[ "$NOT_ROOT_NOTE" = 0 ] || say "Not root: installing for $CUR_USER only. With sudo it would go to /usr/local/bin \
and also set up access to /dev/uinput."
[ "$DRY" = 0 ] || say "Dry run: nothing is changed."

read_record
while IFS= read -r f; do
  [ -z "$f" ] || rec_add "file $f"
done <<EOF
$OLD_FILES
EOF
for u in $OLD_SERVICES; do rec_add "service $u"; done
# --no-udev is remembered: an update ("run it again") leaves /dev/uinput alone until --udev says otherwise
if [ "$MODE" = system ] && [ "$UDEV_SET" = 0 ] && [ "$OLD_NOUDEV" = 1 ]; then
  UDEV=0
  say "Leaving /dev/uinput access alone, as an earlier --no-udev asked (--udev sets it up)."
fi

# the services: asked for now, or installed before (then updated)
SERVICES=
if [ "$SERVICE" = 1 ]; then SERVICES=$SVC_USER; fi
for u in $OLD_SERVICES; do
  case " $SERVICES " in *" $u "*) ;; *) SERVICES="${SERVICES:+$SERVICES }$u" ;; esac
done

# A copy installed for the invoking user without sudo (~/.local/bin) comes first on PATH (and its user unit
# before this one): it would keep running the old version after every update of this one.
if [ "$MODE" = system ] && [ -n "$INVOKER" ] && inv_home=$(user_home "$INVOKER") && [ -n "$inv_home" ]; then
  inv_home=${inv_home%/}
  if [ -f "$ROOT$inv_home/.local/share/$APP/installed" ] || [ -e "$ROOT$inv_home/.local/bin/$APP" ]; then
    warn "$INVOKER also has a copy in $inv_home/.local/bin (installed without sudo): it comes first on PATH, so this \
one would not run. Remove it: sh install.sh --uninstall (as $INVOKER, without sudo)"
  fi
fi

# ---- the binary
if [ "$DRY" = 0 ]; then
  TMPD=$(mktemp -d "${TMPDIR:-/tmp}/elrs-wifi-joystick.XXXXXX") || die "cannot make a temporary directory"
fi
NEWBIN=
VER=
if [ -n "$FILE" ]; then
  if [ "$DRY" = 1 ]; then
    say "  would install $FILE"
  else
    say "Using $FILE"
    cp "$FILE" "$TMPD/$APP" || die "cannot read $FILE"
    NEWBIN=$TMPD/$APP
  fi
elif [ "$DRY" = 1 ]; then
  say "  would download $APP ${WANT:-(the latest release)} for $ARCH from github.com/$REPO and check it against SHA256SUMS"
else
  pick_downloader
  SUMS=$TMPD/SHA256SUMS
  VIA_API=0
  if [ -n "${GITHUB_TOKEN:-}" ] && [ "$DL" = curl ]; then
    # a token (a private fork): the API names the assets; an asset is fetched by its id
    if [ -n "$WANT" ]; then api=https://api.github.com/repos/$REPO/releases/tags/v$WANT
    else api=https://api.github.com/repos/$REPO/releases/latest; fi
    if fetch "$api" "$TMPD/release.json" api; then
      VIA_API=1
    else
      # an expired token, or one exported for another account: the public download needs none
      warn "GITHUB_TOKEN cannot read release ${WANT:-latest} of $REPO (no such release, an expired token, or one \
for another account): trying the public download"
    fi
  fi
  if [ "$VIA_API" = 1 ]; then
    # shellcheck disable=SC2020  # one field per line, whatever the JSON layout
    tr ',{}[]' '\n\n\n\n\n' < "$TMPD/release.json" > "$TMPD/fields"
    tag=$(sed -n 's/^ *"tag_name": *"\([^"]*\)" *$/\1/p' "$TMPD/fields" | head -n 1)
    sed -n 's|^ *"url": *"[^"]*/releases/assets/\([0-9][0-9]*\)" *$|\1|p' "$TMPD/fields" > "$TMPD/ids"
    sed -n 's|^ *"browser_download_url": *"[^"]*/\([^/"]*\)" *$|\1|p' "$TMPD/fields" > "$TMPD/names"
    if [ -z "$tag" ] || [ "$(wc -l < "$TMPD/ids")" -ne "$(wc -l < "$TMPD/names")" ]; then
      die "unexpected answer from the GitHub API for release ${WANT:-latest}"
    fi
    paste -d ' ' "$TMPD/ids" "$TMPD/names" > "$TMPD/assets"
    VER=${tag#v}
    NAME=$APP-$VER-linux-$ARCH
    sums_id=$(awk '$2 == "SHA256SUMS" { print $1; exit }' "$TMPD/assets")
    bin_id=$(awk -v n="$NAME" '$2 == n { print $1; exit }' "$TMPD/assets")
    [ -n "$sums_id" ] || die "release $tag has no SHA256SUMS"
    [ -n "$bin_id" ] || die "release $tag has no $NAME (no binary for $ARCH in that release)"
    say "Downloading $NAME (release $tag)"
    fetch "https://api.github.com/repos/$REPO/releases/assets/$sums_id" "$SUMS" asset || die "cannot download SHA256SUMS of $tag"
    fetch "https://api.github.com/repos/$REPO/releases/assets/$bin_id" "$TMPD/$APP" asset || die "cannot download $NAME"
  else
    tok="a private fork needs GITHUB_TOKEN"
    [ -z "${GITHUB_TOKEN:-}" ] || tok="for a private fork, GITHUB_TOKEN must be able to read it, and curl is needed"
    if [ -n "$WANT" ]; then
      fetch "https://github.com/$REPO/releases/download/v$WANT/SHA256SUMS" "$SUMS" ||
        die "cannot download release v$WANT of $REPO (no such release, or no network; $tok)"
      VER=$WANT
    else
      fetch "https://github.com/$REPO/releases/latest/download/SHA256SUMS" "$SUMS" ||
        die "cannot download the latest release of $REPO (none yet, or no network; $tok)"
      # the asset names carry the version: the checksum list tells which one is the latest
      VER=$(awk '{ f = $2; sub(/^\*/, "", f); print f }' "$SUMS" |
            sed -n "s/^$APP-\\([0-9][0-9A-Za-z.+-]*\\)-linux-$ARCH\$/\\1/p" | head -n 1)
      [ -n "$VER" ] || die "the latest release has no binary for $ARCH"
    fi
    NAME=$APP-$VER-linux-$ARCH
    say "Downloading $NAME"
    fetch "https://github.com/$REPO/releases/download/v$VER/$NAME" "$TMPD/$APP" || die "cannot download $NAME"
  fi
  want_sum=$(awk -v n="$NAME" '{ f = $2; sub(/^\*/, "", f); if (f == n) { print $1; exit } }' "$SUMS")
  case $want_sum in
    *[!0-9a-fA-F]*|'') die "SHA256SUMS has no checksum for $NAME" ;;
  esac
  [ ${#want_sum} -eq 64 ] || die "SHA256SUMS has no valid checksum for $NAME"
  got_sum=$(sha256_of "$TMPD/$APP") || exit 1
  if [ "$(printf '%s' "$got_sum" | tr 'A-F' 'a-f')" != "$(printf '%s' "$want_sum" | tr 'A-F' 'a-f')" ]; then
    die "the download of $NAME does not match its checksum in SHA256SUMS (a broken download?) - nothing was installed"
  fi
  say "Checksum OK ($got_sum)"
  NEWBIN=$TMPD/$APP
fi

if [ -n "$NEWBIN" ]; then
  # it must run here, unless it is meant for another machine (--root, --arch)
  chmod 0755 "$NEWBIN"
  rv=0
  out=$("$NEWBIN" --version 2>&1) || rv=$?
  if [ "$rv" = 126 ]; then
    # not executable from $TMPDIR (mounted noexec on hardened systems): try again from where it goes
    mkdirs "$BIN_DIR"
    PENDING=$ROOT$BIN_DIR/.$APP.check.$$
    if cp "$NEWBIN" "$PENDING" 2>/dev/null && chmod 0755 "$PENDING"; then
      rv=0
      out=$("$PENDING" --version 2>&1) || rv=$?
    fi
    rm -f "$PENDING"
    PENDING=
  fi
  ver=${out%%"$NL"*}           # its first line: "elrs-wifi-joystick <version>" (then the project's links)
  if [ "$rv" = 0 ] && [ "${ver%% *}" = "$APP" ]; then
    say "Version: ${ver#"$APP "}"
    [ -z "$VER" ] || [ "${ver#"$APP "}" = "$VER" ] || warn "the release is $VER but the program says ${ver#"$APP "}"
  elif [ -z "$ROOT" ] && [ -z "$ARCH_OPT" ]; then
    why=${out:-no output}
    [ "$rv" != 126 ] || why="$why - it may not be executed there: is $ROOT$BIN_DIR on a file system mounted noexec?"
    die "the program does not run on this machine ($why) - nothing was installed"
  else
    say "(not run here: it may be for another machine)"
  fi
  if [ -x "$ROOT$BIN" ] && old=$("$ROOT$BIN" --version 2>/dev/null) && old=${old%%"$NL"*} && [ "${old%% *}" = "$APP" ]; then
    say "Replacing ${old#"$APP "} in $ROOT$BIN"
  fi
fi
# The record first, with everything this run will install: an install that stops halfway (Ctrl-C, a full
# disk, a later error) leaves nothing that --uninstall does not know about. --uninstall skips what is not there.
rec_add "file $BIN"
if [ "$UDEV" = 1 ] && [ "$MODE" = system ]; then
  rec_add "file $RULE"
  rec_add "file $MODLOAD"
fi
[ "$MODE" != system ] || [ "$UDEV" = 1 ] || rec_add no-udev
if [ -n "$SERVICES" ]; then
  rec_add "file $UNIT"
  for u in $SERVICES; do rec_add "service $u"; done
fi
write_record() {
  if [ "$DRY" = 1 ]; then return 0; fi
  put_text "$REC" 0644 "# elrs-wifi-joystick install.sh: what it installed (sh install.sh --uninstall removes it)$REC_LINES"
}
write_record

if [ "$DRY" = 1 ]; then
  say "  would install $ROOT$BIN"
else
  put_file "$NEWBIN" "$BIN" 0755
  say "Installed $ROOT$BIN"
fi

# ---- /dev/uinput access and the uinput module
UINPUT_NOTE=
if [ "$UDEV" = 1 ] && [ "$MODE" = system ]; then
  put_text "$RULE" 0644 "$RULE_TEXT"
  put_text "$MODLOAD" 0644 "$MODLOAD_TEXT"
  [ "$DRY" = 1 ] || say "Installed $ROOT$RULE and $ROOT$MODLOAD"
  if [ "$LIVE" = 1 ]; then
    if MODPROBE=$(find_cmd modprobe); then
      "$MODPROBE" uinput || warn "modprobe uinput failed: this kernel may have no uinput module (the program then runs as a monitor only)"
    else
      warn "there is no modprobe here: load uinput by hand, or reboot"
    fi
    if UDEVADM=$(find_cmd udevadm) && "$UDEVADM" control --reload 2>/dev/null; then
      "$UDEVADM" trigger --action=change --subsystem-match=misc --sysname-match=uinput 2>/dev/null || true
      "$UDEVADM" settle --timeout=10 2>/dev/null || true
    else
      warn "udev is not running here (a container?): the rule applies after a reboot"
    fi
    if [ -c /dev/uinput ]; then
      say "/dev/uinput: $(ls -l /dev/uinput 2>/dev/null)"
    else
      warn "/dev/uinput does not exist: the program cannot make its joystick until uinput is loaded"
    fi
    have_group input || warn "this system has no \"input\" group: only the user logged in at the screen gets access"
  else
    skipped "modprobe uinput"
    skipped "udevadm control --reload && udevadm trigger --action=change --subsystem-match=misc --sysname-match=uinput"
  fi
  NOTE_USER=$INVOKER
  if [ -n "$NOTE_USER" ] && [ "$LIVE" = 1 ] && ! in_group "$NOTE_USER" input; then
    UINPUT_NOTE="At the screen (a desktop session), $NOTE_USER now has access to /dev/uinput. Over SSH, or for a
  service started at boot, add $NOTE_USER to the input group (then log in again):
    sudo usermod -aG input $NOTE_USER"
  fi
elif [ "$UDEV" = 1 ]; then
  if [ "$LIVE" = 1 ] && [ -c /dev/uinput ] && [ -w /dev/uinput ]; then
    UINPUT_NOTE="You can use /dev/uinput already: nothing else needs root."
  else
    rule_line=$(printf '%s\n' "$RULE_TEXT" | grep '^KERNEL==')
    UINPUT_NOTE="To let the program make its virtual joystick, give access to /dev/uinput once (needs root):
    printf '%s\\n' '$rule_line' | sudo tee $RULE >/dev/null
    echo uinput | sudo tee $MODLOAD >/dev/null
    sudo modprobe uinput && sudo udevadm control --reload && sudo udevadm trigger --action=change --subsystem-match=misc --sysname-match=uinput
  (or remove this copy - sh install.sh --uninstall - and run this script with sudo, which installs to
  /usr/local/bin and does this too). Over SSH or for a service started at boot, also:
    sudo usermod -aG input $CUR_USER   (then log in again)"
  fi
fi

# ---- the systemd user service (asked for now, or installed before: then it is updated)
SVC_NOTE=
svc_first=${SERVICES%% *}
if [ -n "$SERVICES" ]; then
  unit_text=$(printf '%s\n' "$UNIT_TEXT" | sed "s|^ExecStart=/usr/local/bin/$APP |ExecStart=$EXEC_PATH |")
  put_text "$UNIT" 0644 "$unit_text"
  [ "$DRY" = 1 ] || say "Installed $ROOT$UNIT"
  for u in $SERVICES; do
    asked=0
    [ "$SERVICE" = 0 ] || [ "$u" != "$SVC_USER" ] || asked=1
    if [ "$LIVE" = 0 ]; then
      if [ "$asked" = 1 ]; then skipped "as $u: systemctl --user daemon-reload && systemctl --user enable $UNIT_NAME && systemctl --user restart $UNIT_NAME"
      else skipped "as $u: systemctl --user daemon-reload && systemctl --user try-restart $UNIT_NAME"; fi
    elif has_session "$u"; then
      user_systemctl "$u" daemon-reload || warn "systemctl --user daemon-reload failed for $u"
      if [ "$asked" = 1 ]; then
        if user_systemctl "$u" enable "$UNIT_NAME" && user_systemctl "$u" restart "$UNIT_NAME"; then
          say "Service enabled and started for $u: $(user_systemctl "$u" is-active "$UNIT_NAME" 2>/dev/null || true)"
        else
          warn "could not enable or start the service for $u"
        fi
      else
        user_systemctl "$u" try-restart "$UNIT_NAME" || warn "could not restart the service of $u"
      fi
    elif [ "$asked" = 1 ]; then
      # no user manager runs now: enable it the way systemctl does (the link), it starts at the next login
      wl=$(wants_link "$u") || die "cannot find the home directory of $u"
      if as_user "$u" mkdir -p "${wl%/*}" && as_user "$u" ln -sf "$UNIT" "$wl"; then
        if [ -d /run/systemd/system ]; then say "Service enabled for $u: it starts when $u logs in."
        else say "Service enabled for $u: it starts when $u logs in with systemd running (it does not run here now)."
        fi
      else
        warn "could not enable the service for $u: log in as $u and run: systemctl --user enable --now $UNIT_NAME"
      fi
    fi
  done
  SVC_NOTE="The service: systemctl --user status $UNIT_NAME   (log: journalctl --user -u $UNIT_NAME -f)
  It runs while its user is logged in; to run it from boot without a login: sudo loginctl enable-linger $svc_first"
fi

# ---- the record (as written before the files; again, so it is the last word)
if [ "$DRY" = 1 ]; then
  say "  would write $ROOT$REC"
else
  write_record
  sync 2>/dev/null || :            # the renames too
fi

# ---- next steps
if [ "$DRY" = 1 ]; then exit 0; fi
say ""
say "Next:"
if [ "$MODE" = user ]; then
  case ":${PATH:-}:" in
    *":$BIN_DIR:"*) ;;
    *) say "  $BIN_DIR is not on your PATH: run it as $BIN, or add the folder to PATH (e.g. in ~/.profile)." ;;
  esac
fi
say "  1. Put the ELRS / Crossfire module on the same WiFi network as this machine."
if [ -n "$SERVICES" ]; then
  say "  2. The service runs the program; or stop it and run $APP in a terminal to watch."
else
  say "  2. Run: $APP          (keys: s start/stop, r rate, c channels, h help, q quit)"
fi
say "  3. Open http://127.0.0.1:11000/ to see the sticks and change the settings."
say "  Firewall: the module sends to UDP port 11000. If a firewall runs here, let it in:"
say "    sudo ufw allow 11000/udp                     (ufw: Ubuntu, Mint, ...)"
say "    sudo firewall-cmd --permanent --add-port=11000/udp && sudo firewall-cmd --reload   (Fedora, openSUSE, ...)"
[ -z "$UINPUT_NOTE" ] || say "  $UINPUT_NOTE"
[ -z "$SVC_NOTE" ] || say "  $SVC_NOTE"
exit 0
