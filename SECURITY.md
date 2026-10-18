# Security

## Reporting a problem

Please do not open a public issue for a security problem. Report it privately through GitHub: **Security → Report a vulnerability** on this repository ([new advisory](https://github.com/mathiasbk/openESL/security/advisories/new)). Say what is affected (AP, tag, web page, tools), the version, and how to reproduce it. You will get an answer as soon as possible; this is a spare-time project, so give it a few days.

## Supported versions

Only the newest release gets fixes. Update the AP and the tags to it before reporting.

## Known limitations

These are known and documented, not secrets:

- **The radio has no security yet.** Frames between the AP and the tags are not authenticated or encrypted (requirement T16 is on hold). Anyone in radio range who speaks the protocol can send content to a tag.
- **Tag firmware over the radio is not signed.** For the same reason, anyone in radio range can install firmware on a tag ([docs/tag-ota.md](docs/tag-ota.md), "Security"). This was chosen on purpose, to have OTA now.
- **The HTTP API is plain HTTP** on the local network, with a bearer token. Keep the AP on a network you trust, or behind a VPN. Five wrong tokens lock the API for 60 seconds.
- **The USB port needs no token.** Anyone with the AP in their hands can change it.

Reports that find a way around the token, the lockout or the AP's own OTA rollback, or that get code run on the AP, are the most useful.
