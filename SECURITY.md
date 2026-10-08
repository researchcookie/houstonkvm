# Security policy

HoustonKVM gives whoever controls it full keyboard and mouse control of a real
machine, so security problems matter here.

## Reporting a vulnerability

**Please don't open a public issue.** Instead use GitHub's private reporting on this
repository: the **Security** tab, then **Report a vulnerability**.

If that isn't available to you, email the maintainer, Laszlo Coleman, at
laszlo.coleman@researchcookie.com.

Helpful things to include: the version (`HoustonKVM --version`), how it is deployed
(behind a proxy or not), and the steps to reproduce.

This is a small project. We will acknowledge a report as soon as we can, work on a
fix with you, and credit you in the release notes if you would like.

## Supported versions

Only the latest release receives security fixes.

## What is and isn't a vulnerability

Some behaviour is a known limitation rather than a bug, and is described in the
[security guide](docs/security.md): the server starts on plain
HTTP until an Owner turns on HTTPS, it is meant for trusted networks, and there is
no two-factor sign-in yet. Reports that improve on those are
welcome as ordinary issues.

Things we would very much like to hear about privately include: getting control of
a target without the Operator role, reading device paths or other configuration as
a lower role, bypassing the sign-in lockout, keys or buttons left held on a target
after control ends, reading an HTTPS private key or changing HTTPS without the
Owner role, and any way to run code on the server.
