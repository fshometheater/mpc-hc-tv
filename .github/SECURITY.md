# Security policy

## Reporting a problem

Please **don't report security problems in public issues.** Report them privately instead:

1. Open this repository's **Security** tab.
2. Click **Report a vulnerability** and describe the problem: what you found, which version (Help >
   About), and how to reproduce it if you can.

Only the maintainer sees the report. You'll get an answer there, and a fix will be released as a new
version with a note in the release.

## Supported versions

Only the **latest release** on the [releases page](https://github.com/fshometheater/mpc-hc-tv/releases)
gets fixes. This is a small personal project maintained in spare time, so there is no fixed response time.

## What belongs here

- **This project's own code:** TV Mode (`src/mpc-hc/Tv*`), the small `TV MODE` changes in MPC-HC's
  files, the installer and the release packages.
- **MPC-HC itself:** problems that also happen in the official MPC-HC belong to the
  [MPC-HC project](https://github.com/clsid2/mpc-hc).
- **LAV Filters, MediaInfo, MPC Video Renderer:** these ship unchanged from the official MPC-HC
  release; report problems in them to their own projects.
- **madVR** is not part of this project.

## Downloads

The release files are not code-signed (neither are MPC-HC's), so Windows SmartScreen may warn on the
first start. Each release lists the SHA-256 of its files; to check a download, run `Get-FileHash <file>`
in PowerShell and compare. Only download from this repository's releases page.
