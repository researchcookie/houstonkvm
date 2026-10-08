# Examples

For HTTPS managed outside the web interface. The packaged copies are in
`/usr/share/doc/houstonkvm/examples/`, and `man houstonkvm` has the options and
files they rely on.

| File | What it does |
|---|---|
| [houstonkvm-https.yml](houstonkvm-https.yml) | Ansible: installs HoustonKVM on a group of hosts with a certificate from the company's CA in `/etc/houstonkvm/tls/`, HTTPS pinned on (`--tls=on`), and the firewalld service open. Run it again to roll out renewed certificates; sessions carry on. |
| [certbot-deploy-hook-files.sh](certbot-deploy-hook-files.sh) | certbot deploy hook: copies a renewed certificate into `/etc/houstonkvm/tls/` and runs `systemctl reload houstonkvm`. The certificate is then the configuration's, read-only in Admin. |
| [certbot-deploy-hook-api.sh](certbot-deploy-hook-api.sh) | certbot deploy hook: uploads a renewed certificate with an Owner's API token (`POST /api/tls/certificate`). It stays the Owner's, and certbot can run on another machine. `tests/integration/test_tls.py` runs this one. |

certbot isn't only for Let's Encrypt: `certbot --server` works with an internal
ACME server too (step-ca, for example). Without one, HoustonKVM's own
certificate authority (Admin → Network → HTTPS) renews its certificates by
itself.
