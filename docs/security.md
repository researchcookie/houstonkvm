# Security


Security is your responsibility. HoustonKVM gives whoever controls it
**full keyboard and mouse control of a real machine**

- **Turn on HTTPS.** HoustonKVM starts on plain HTTP, and Admin says so until you
  change it. Under **Admin → Network → HTTPS** an Owner first gets a certificate:
  from the organization's own CA with a signing request (the key never leaves the
  server) or by uploading one, or from HoustonKVM's own small certificate authority
  (trust it once, in browsers or by GPO or Ansible, and the certificate renews
  itself). Then they turn HTTPS on. No restart. The
  switch only sticks once a browser reaches the HTTPS address, so a blocked port
  or a refused certificate puts the server back on plain HTTP after two minutes.
  Open 8443/tcp too (the `houstonkvm` firewalld service covers it).
  - **Locked out anyway?** Add `--tls=off` to `/etc/houstonkvm/houstonkvm.conf` and
    restart. That turns HTTPS off, and it stays off when you remove the option
    again, so fix things in Admin over plain HTTP and turn HTTPS on there.
  - **Fleets:** `--tls=on` and a certificate in `/etc/houstonkvm/tls/` fix HTTPS in
    the configuration instead, and `systemctl reload houstonkvm` picks up a
    renewed certificate without dropping anyone. See `man houstonkvm` and the
    [examples](examples/) (an Ansible playbook and certbot hooks).
  - The system's crypto policy decides TLS versions and ciphers, and certificates
    are made with OpenSSL's FIPS-compatible interface.
- **Or put a reverse proxy in front.** Start the server with
  `--bind=127.0.0.1 --tls=off --secure-cookies --trusted-proxy=127.0.0.1` so only
  the proxy can reach it, and make the proxy pass the browser's `Host` header and
  WebSocket upgrades, add `X-Forwarded-For`, and not buffer the video streams.
  Video over WebRTC travels straight to the server over UDP and does not go
  through the proxy. MJPEG video does, and a viewer on a slow link can fall
  further behind than without the proxy: frames the server would skip for them sit
  in the proxy's buffers instead. For nginx (`tests/integration/test_reverse_proxy.py`
  runs this block as written):

  ```nginx
  location / {
      proxy_pass http://127.0.0.1:8080;
      proxy_http_version 1.1;
      proxy_set_header Host $http_host;
      proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
      proxy_set_header Upgrade $http_upgrade;
      proxy_set_header Connection "upgrade";
      proxy_buffering off;
  }
  ```

- **Never expose HoustonKVM to the open Internet.** Always keep it on a trusted network or behind a VPN.
- **Sign-in is rate limited per source address**, five failures locking that address
  out for five minutes. Behind a proxy, name it with `--trusted-proxy` so each
  visitor's own address (from `X-Forwarded-For`) is the one counted; otherwise every
  visitor shares the proxy's address and one person's typos lock everyone out.
- **Requests that use the session cookie must come from the server's own page.** A
  page on another site, or on another port of the same host, can't drive a target
  with your cookie: the server checks the browser's `Origin` against the `Host`
  header. If your proxy rewrites `Host`, tell the server its public address with
  `--allowed-origin=https://kvm.example.com`. The UI also ships a strict
  Content-Security-Policy, so it runs no script but its own files.
- **There is no two-factor sign-in yet.**
- **There is an audit log** (Admin → Audit, or the [API](API.md#audit-log)):
  sign-ins, account, token and target changes, and every time someone drove a
  target: who, from where, for how long, and how many keys and mouse events they
  sent. **Never what they typed**, so passwords typed into a target don't end up
  in the log. Only Owners can read it. Records are kept for 90 days
  (`--audit-retention-days`).
- **API tokens** can do at most what the account that made them can. Give each
  one only what it needs: a `read` token can't drive a target or change
  anything, and a `metrics` token can only read `/metrics`. Tokens expire after
  90 days unless you choose otherwise. Revoke ones you don't use.

To report a vulnerability, see [SECURITY.md](../SECURITY.md).
