#pragma once

#include "auth/auth.h"
#include "core/database.h"
#include "tls/tls_manager.h"

#include <App.h>

namespace houston_kvm {

// HTTPS, for an Owner (a browser session or an API token, so a certbot
// deploy hook can install renewals):
//   GET  /api/tls              mode, settings, certificate details (never the key)
//   PUT  /api/tls/settings     {https_port, http_redirect, hsts, auto_renew}, applied live
//   POST /api/tls/certificate  {cert, key} (PEM), or {cert} for the pending CSR;
//                              swapped in live
//   POST /api/tls/generate     {names, key_type}: issued by HoustonKVM's own CA
//   POST /api/tls/csr          {names, key_type, organization, unit, country}
//   GET  /api/tls/csr          the pending CSR (PEM); DELETE discards it
//   POST /api/tls/enable       starts HTTPS beside HTTP; returns the confirm code
//   POST /api/tls/disable      back to plain HTTP (or cancels a pending enable)
// for anyone signed in:
//   GET  /api/tls/ca.crt       HoustonKVM's own CA certificate, to trust
// and, with no sign-in, over HTTPS only:
//   POST /api/tls/confirm      {code}: proof a browser reached HTTPS, which
//                              makes it stick
// See TlsManager for what each does.
template <bool SSL>
void registerTlsRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db, TlsManager*& tls);

} // namespace houston_kvm
