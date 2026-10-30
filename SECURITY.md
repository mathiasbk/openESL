# Security

## Reporting a problem

Please do not open a public issue for a security problem. Report it privately through GitHub: **Security → Report a vulnerability** on this repository ([new advisory](https://github.com/mathiasbk/openESL/security/advisories/new)). Say what is affected (AP, tag, web page, tools), the version, and how to reproduce it. You will get an answer as soon as possible; this is a spare-time project, so give it a few days.

## Supported versions

Only the newest release gets fixes. Update the AP and the tags to it before reporting.

## Known limitations

These are known and documented, not secrets:

- **The radio is signed only for paired tags, and not encrypted.** A paired tag signs its messages and takes only answers signed with its key (AES-CMAC, [docs/protokoll.md](docs/protokoll.md), "Security"); tags that are not paired work as before, and with "Only paired tags" off the AP still answers them. Anyone in range can read what is sent.
- **Pairing sends the key once in the clear.** A tag without a key gets it unsigned at its next poll. Pair where nobody listens in.
- **Tag firmware is checked per block, not signed as an image.** A paired tag takes firmware only from its AP, block by block; a tag that is not paired takes it from anyone in radio range ([docs/tag-ota.md](docs/tag-ota.md), "Security").
- **The radio key is in the backup.** Keep backup files as secret as the AP.
- **The HTTP API is plain HTTP** on the local network, with a bearer token. Keep the AP on a network you trust, or behind a VPN. Five wrong tokens lock the API for 60 seconds.
- **The USB port needs no token.** Anyone with the AP in their hands can change it.

Reports that find a way around the token, the lockout or the AP's own OTA rollback, or that get code run on the AP, are the most useful.
