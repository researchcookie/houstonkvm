#!/bin/bash
# certbot deploy hook: hands a renewed certificate to HoustonKVM as files.
#
# HoustonKVM serves /etc/houstonkvm/tls/houstonkvm.crt and houstonkvm.key when
# both exist, and re-reads them on `systemctl reload houstonkvm`: new
# connections get the new certificate, and open sessions, video and control
# carry on. Admin then shows the certificate as managed by the configuration.
#
# Install as root:
#   install -m 0755 certbot-deploy-hook-files.sh /etc/letsencrypt/renewal-hooks/deploy/houstonkvm.sh
#   certbot certonly ... -d kvm.example.com      # the first certificate
#   RENEWED_LINEAGE=/etc/letsencrypt/live/kvm.example.com \
#       /etc/letsencrypt/renewal-hooks/deploy/houstonkvm.sh
#   systemctl restart houstonkvm                 # once: the files are found at start
#
# The ACME server needn't be Let's Encrypt: certbot --server works with an
# internal one (step-ca, an Active Directory CS ACME gateway, ...).
set -euo pipefail

# Only for the certificate HoustonKVM uses, when certbot manages several.
DOMAIN=${HOUSTONKVM_DOMAIN:-}
if [[ -n $DOMAIN && ${RENEWED_LINEAGE:?} != */live/$DOMAIN ]]; then
    exit 0
fi

dir=/etc/houstonkvm/tls
# Written beside the old files, then renamed over them, so the server never
# reads half a file. Even a mismatched pair would do no harm: a reload that
# can't load the pair keeps serving the one in use.
install -o root -g houstonkvm -m 0640 "${RENEWED_LINEAGE:?}/privkey.pem" "$dir/.houstonkvm.key.new"
install -o root -g houstonkvm -m 0640 "$RENEWED_LINEAGE/fullchain.pem" "$dir/.houstonkvm.crt.new"
mv -f "$dir/.houstonkvm.key.new" "$dir/houstonkvm.key"
mv -f "$dir/.houstonkvm.crt.new" "$dir/houstonkvm.crt"
restorecon -F "$dir/houstonkvm.key" "$dir/houstonkvm.crt" 2>/dev/null || true

systemctl reload houstonkvm
