# Verified OPT17 release upload transport

This isolated branch transports already verified archives to the existing draft release. It has no game source ancestry and does not change the source branch. The upload workflow verifies the split files and reconstructed archive before uploading; publication is a separate verified step.

Release: https://github.com/NeoniusNext/SRB2-Banpyura-PS2/releases/tag/pak-opt17-20261116

The main ZIP is split only to fit GitHub's per-file Git limit. The release receives the original single ZIP, byte for byte. To reconstruct manually, download both parts and run:

```sh
sha256sum --check PARTS.sha256
cat SRB2-PS2-PAKs-OPT17-20261116.zip.part01 SRB2-PS2-PAKs-OPT17-20261116.zip.part02 > SRB2-PS2-PAKs-OPT17-20261116.zip
sha256sum --check SHA256SUMS
```

On Windows: `copy /b SRB2-PS2-PAKs-OPT17-20261116.zip.part01+SRB2-PS2-PAKs-OPT17-20261116.zip.part02 SRB2-PS2-PAKs-OPT17-20261116.zip`.

Source, complete verification reports, exact pack sizes and optional TEXC RAM costs: https://github.com/NeoniusNext/SRB2-Banpyura-PS2/tree/3aad51501a95cf39071870ca2cfa067d886d3102/prebuilt/pak-opt17-20261116
