#!/bin/bash
# certbot deploy hook: uploads a renewed certificate through HoustonKVM's API.
#
# Unlike the files hook, the certificate stays an Owner's: Admin can still
# replace it. It suits a certbot running on another machine, too. It takes an
# API token made by an Owner (Admin -> Users -> API tokens), kept root-only:
#   install -m 0600 /dev/stdin /etc/houstonkvm/certbot-token <<< 'TOKEN'
#   install -m 0755 certbot-deploy-hook-api.sh /etc/letsencrypt/renewal-hooks/deploy/houstonkvm.sh
#
# The key crosses the network, so HOUSTONKVM_URL must be HTTPS, or plain HTTP
# to this machine only (http://127.0.0.1:8080 while HTTPS is off).
set -euo pipefail

NAME=${HOUSTONKVM_DOMAIN:?set HOUSTONKVM_DOMAIN, e.g. kvm.example.com}
URL=${HOUSTONKVM_URL:-https://$NAME:8443}
TOKEN_FILE=${HOUSTONKVM_TOKEN_FILE:-/etc/houstonkvm/certbot-token}

if [[ ${RENEWED_LINEAGE:?} != */live/$NAME ]]; then
    exit 0
fi

# {"cert": the chain, "key": its key}, built without the key ever being on a
# command line.
body=$(python3 - "$RENEWED_LINEAGE" <<'PY'
import json, sys
d = sys.argv[1]
print(json.dumps({"cert": open(d + "/fullchain.pem").read(),
                  "key": open(d + "/privkey.pem").read()}))
PY
)

# Talks to this machine under the certificate's name, so curl checks the
# certificate in use against the system's trusted CAs as usual.
resolve=()
[[ $URL == https://$NAME:* ]] && resolve=(--resolve "$NAME:${URL##*:}:127.0.0.1")

# The reply, or why it was refused, goes to certbot's log; a refusal fails
# the hook, so certbot reports it.
status=0
curl --fail-with-body --silent --show-error "${resolve[@]}" \
     -H @<(printf 'Authorization: Bearer %s\n' "$(cat "$TOKEN_FILE")") \
     -H 'Content-Type: application/json' \
     --data-binary @- "$URL/api/tls/certificate" <<< "$body" || status=$?
echo
exit "$status"
