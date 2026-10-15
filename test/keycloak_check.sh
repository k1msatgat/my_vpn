#!/usr/bin/env bash
# 실제 IdP(Keycloak)가 발급한 토큰을 서버가 받는지 확인한다.
#
#   KC=http://localhost:8080 test/keycloak_check.sh
#
# Keycloak 이 떠 있어야 한다 (관리자 admin / admin). CI 의 idp 잡이 컨테이너로 띄운다:
#
#   docker run -d -p 8080:8080 -e KC_BOOTSTRAP_ADMIN_USERNAME=admin \
#       -e KC_BOOTSTRAP_ADMIN_PASSWORD=admin quay.io/keycloak/keycloak:26.0.7 start-dev
#
# 다른 시험(auth_check.py, linux_e2e.sh)의 토큰은 tools/idp/mint.py 가 만든다. 서명과 검증의
# 논리는 그것으로 확인되지만, "실제 IdP 의 토큰이 이 모양인가"는 확인되지 않는다 — aud 가
# 배열로 오는지, JWKS 에 서명용이 아닌 키가 섞여 있는지, 그룹 클레임이 어떤 이름으로 오는지.
# 여기서는 README 의 "사용자 로그인"에 적은 Keycloak 설정을 관리 API 로 그대로 만들고, 그
# 설정이 발급한 토큰을 vpn_server verify-token 에 넣어 본다.
#
#   1. 렐름, 공개 클라이언트(my_vpn, 기기 인가 켬, audience / 그룹 매퍼), 그룹 ops, 사용자 alice
#   2. 디스커버리에서 issuer 와 jwks_uri 를 읽어 idp.conf 와 jwks.json 을 만든다
#   3. alice 의 토큰 -> ok user=alice groups=ops
#   4. 같은 IdP 가 다른 클라이언트(other)에 준 토큰 -> wrong_token (aud)
#   5. 서명 뒤에 바꾼 토큰 -> untrusted
#   6. 기기 인가 엔드포인트가 기기 코드를 준다 (client/my_vpn_login 이 쓰는 첫 단계)
#
# 토큰은 비밀번호 방식(direct access grant)으로 받는다. 기기 인가는 사람이 브라우저에서
# 승인해야 끝나므로 자동 시험에서는 첫 단계까지만 본다. 나머지 흐름은 linux_e2e.sh 가
# 시험용 OIDC 서버로 확인한다.

set -u

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SERVER="$ROOT_DIR/server/bin/vpn_server"
KC=${KC:-http://localhost:8080}
REALM=my_vpn
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
failed=0

[ -x "$SERVER" ] || { echo "build first: make server" >&2; exit 1; }

check() {	# check <이름> <받은 값> <바라는 값>
	if [ "$2" = "$3" ]; then
		printf 'ok   %-52s %s\n' "$1" "$2"
	else
		printf 'FAIL %-52s %s   (want %s)\n' "$1" "$2" "$3"
		failed=$((failed + 1))
	fi
}

json() {	# json <파이썬 식>   표준 입력의 JSON 을 j 로 읽어 식의 값을 찍는다
	python3 -c "import json, sys; j = json.load(sys.stdin); print($1)"
}

for i in $(seq 90); do
	curl -sf "$KC/realms/master" > /dev/null && break
	sleep 2
done
curl -sf "$KC/realms/master" > /dev/null || { echo "Keycloak is not up at $KC" >&2; exit 1; }

ADMIN=$(curl -sf -d client_id=admin-cli -d username=admin -d password=admin -d grant_type=password \
	"$KC/realms/master/protocol/openid-connect/token" | json 'j["access_token"]')
api() {	# api <method> <path> [json]
	curl -sf -X "$1" -H "Authorization: Bearer $ADMIN" -H 'Content-Type: application/json' \
		"$KC/admin/realms$2" ${3:+-d "$3"}
}

# ---- 1. Keycloak 설정 (README "사용자 로그인" 의 순서 그대로) --------------------------
api POST "" "{\"realm\":\"$REALM\",\"enabled\":true}" || { echo "cannot create realm" >&2; exit 1; }

# 공개 클라이언트: 비밀이 없다 (사용자의 장비에 비밀을 둘 수 없다). 기기 인가를 켠다.
# audience 매퍼가 토큰의 aud 에 my_vpn 을 넣고, 그룹 매퍼가 groups 클레임에 그룹 이름을 넣는다.
api POST "/$REALM/clients" '{
	"clientId": "my_vpn", "publicClient": true, "standardFlowEnabled": false,
	"directAccessGrantsEnabled": true,
	"attributes": {"oauth2.device.authorization.grant.enabled": "true"},
	"protocolMappers": [
		{"name": "audience", "protocol": "openid-connect", "protocolMapper": "oidc-audience-mapper",
		 "config": {"included.client.audience": "my_vpn", "access.token.claim": "true", "id.token.claim": "false"}},
		{"name": "groups", "protocol": "openid-connect", "protocolMapper": "oidc-group-membership-mapper",
		 "config": {"claim.name": "groups", "full.path": "false", "access.token.claim": "true",
		            "id.token.claim": "false", "userinfo.token.claim": "false"}}
	]}' || { echo "cannot create client my_vpn" >&2; exit 1; }

# 같은 IdP 의 다른 앱. 이 앱에 준 토큰으로 VPN 에 들어오면 안 된다.
api POST "/$REALM/clients" '{"clientId": "other", "publicClient": true, "standardFlowEnabled": false,
	"directAccessGrantsEnabled": true}' || { echo "cannot create client other" >&2; exit 1; }

api POST "/$REALM/groups" '{"name": "ops"}' || { echo "cannot create group" >&2; exit 1; }
api POST "/$REALM/users" '{"username": "alice", "enabled": true, "email": "alice@example.test",
	"emailVerified": true, "firstName": "Alice", "lastName": "Test", "groups": ["/ops"],
	"credentials": [{"type": "password", "value": "alice-pw", "temporary": false}]}' \
	|| { echo "cannot create user" >&2; exit 1; }

# ---- 2. 서버 쪽 설정 ----------------------------------------------------------------------
curl -sf "$KC/realms/$REALM/.well-known/openid-configuration" > "$WORK/discovery.json"
ISSUER=$(json 'j["issuer"]' < "$WORK/discovery.json")
curl -sf "$(json 'j["jwks_uri"]' < "$WORK/discovery.json")" > "$WORK/jwks.json"
cat > "$WORK/idp.conf" <<EOF
issuer        $ISSUER
audience      my_vpn
jwks          jwks.json
user-claim    preferred_username
groups-claim  groups
EOF
echo "     issuer $ISSUER, $(json 'len(j["keys"])' < "$WORK/jwks.json") keys in the JWKS" \
	"($(json '",".join(k.get("use", "?") + ":" + k.get("alg", "?") for k in j["keys"])' < "$WORK/jwks.json"))"

token() {	# token <client-id>
	curl -sf -d client_id="$1" -d username=alice -d password=alice-pw -d grant_type=password \
		-d scope=openid "$KC/realms/$REALM/protocol/openid-connect/token" | json 'j["access_token"]'
}
verify() { "$SERVER" verify-token "$WORK/idp.conf" | sed 's/ expires_in=.*//'; }

# ---- 3 ~ 5. 토큰 -------------------------------------------------------------------------
TOK=$(token my_vpn)
echo "     claims: $(echo "$TOK" | cut -d. -f2 | python3 -c 'import base64, json, sys
s = sys.stdin.read().strip(); j = json.loads(base64.urlsafe_b64decode(s + "=" * (-len(s) % 4)))
print({k: j.get(k) for k in ("iss", "aud", "preferred_username", "groups", "azp")})')"
check "keycloak: alice's token for my_vpn" "$(echo "$TOK" | verify)" "ok user=alice groups=ops"
check "keycloak: a token the same IdP gave another app" "$(token other | verify)" "rejected: wrong_token"
TAMPERED=$(echo "$TOK" | python3 -c 'import base64, json, sys
h, p, s = sys.stdin.read().strip().split(".")
j = json.loads(base64.urlsafe_b64decode(p + "=" * (-len(p) % 4)))
j["preferred_username"] = "admin"
print(h + "." + base64.urlsafe_b64encode(json.dumps(j).encode()).rstrip(b"=").decode() + "." + s)')
check "keycloak: alice's token with the user changed" "$(echo "$TAMPERED" | verify)" "rejected: untrusted"

# ---- 6. 기기 인가의 첫 단계 -------------------------------------------------------------
DEVICE=$(json 'j.get("device_authorization_endpoint", "")' < "$WORK/discovery.json")
check "keycloak: discovery offers device authorization" "$([ -n "$DEVICE" ] && echo yes || echo no)" yes
GOT=$(curl -sf -d client_id=my_vpn -d scope=openid "$DEVICE" | json '"device_code" in j and "user_code" in j')
check "keycloak: device authorization hands out a code" "$GOT" True

echo
if [ "$failed" -eq 0 ]; then
	echo "all passed"
else
	echo "$failed failed"
fi
exit "$failed"
