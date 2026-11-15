#!/bin/sh
set -eu
umask 077
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
DEPS=${QDC_DEPS:-$HOME/.cache/qdc507-native-c/host/prefix}
TASK_GO_ROOT=${QDC_GO_ROOT:-}
TASK_GO=${QDC_GO:-go}
TASK_SAN_FLAGS=""
if [ "${QDC_SANITIZE:-0}" = 1 ]; then TASK_SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"; fi
WORK=$(mktemp -d "${TMPDIR:-/tmp}/qdc-auth-XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
cp "$ROOT/auth.go" "$ROOT/persistence.go" "$WORK/"
cat > "$WORK/fixture.go" <<'GO'
package main
import("os";"time";"fmt")
type browserLoginTicket struct {parent,identity string; expires time.Time}
func main(){base:=os.Args[1];mode:=os.Args[2];id:="iccid:8900000000000000001"
 if mode=="generate" {d:=base+"/go-generated";if e:=os.MkdirAll(d,0700);e!=nil{panic(e)};a:=LoadAuth(d);if e:=a.bindIdentity(id);e!=nil{panic(e)};t,e:=a.Setup(id,"460001234567890","+8613800000000","中\x00文ab",false);if e!=nil{panic(e)};if e=os.WriteFile(d+"/go-token.txt",[]byte(t),0600);e!=nil{panic(e)}
 }else {d:=base+"/c-generated";a:=LoadAuth(d);if _,e:=a.Login(id,"汉\x00字ab",false);e!=nil{panic(e)};t,e:=os.ReadFile(d+"/c-token.txt");if e!=nil||!a.Check(string(t),id){panic("C ordinary token cannot reload in Go")};fmt.Println("Go reads C auth: PASS")}
}
GO
run_go_fixture() {
  if [ -n "$TASK_GO_ROOT" ]; then
    env GOENV=off GOROOT="$TASK_GO_ROOT" GOTOOLCHAIN=local GOWORK=off GOPROXY=off GOSUMDB=off GO111MODULE=off "$TASK_GO" run auth.go persistence.go fixture.go "$WORK" "$1"
  else
    env -u GOROOT GOENV=off GOTOOLCHAIN=local GOWORK=off GOPROXY=off GOSUMDB=off GO111MODULE=off "$TASK_GO" run auth.go persistence.go fixture.go "$WORK" "$1"
  fi
}
(cd "$WORK" && run_go_fixture generate)
set -- "$ROOT/native-c/src/auth.c" "$ROOT/native-c/src/clock.c" "$ROOT/native-c/src/common.c"
if [ -f "$ROOT/native-c/src/crypto.c" ]; then set -- "$@" "$ROOT/native-c/src/crypto.c"; fi
${CC:-cc} $TASK_SAN_FLAGS -std=c11 -Wall -Wextra -Wpedantic -g -O1 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I"$ROOT/native-c/include" -I"$DEPS/include" "$ROOT/native-c/tests/test_auth.c" \
  "$@" \
  -L"$DEPS/lib" -lmbedtls -lmbedx509 -lmbedcrypto -lyyjson -pthread -lm -o "$WORK/test_auth"
"$WORK/test_auth" "$WORK"
(cd "$WORK" && run_go_fixture verify)
