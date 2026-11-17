# TC1 OTA internals and custom images

**For TC1 A1 hardware (EMW3031 / MK3031) running stock firmware 1.0.0.26, 1.0.0.24 or 1.0.0.21.** Other stock versions, TC1 A2 and the A1 air purifier are not supported.

This page covers factory OTA internals, image packaging and alternative topologies. To follow the procedure, use the [illustrated OTA guide](tc1-no-disassembly-ethernet-wifi-ap), which carries the files and the Windows / macOS / Linux steps.

The bundled `mico_ota.bin` is a fixed **0.9.6-ed4474 migration image** for stock installation only; it does not track current releases. After migration, upgrade to the latest compatible stable TC1 release from [Downloads]({{SITE_URL}}en/#releases) as described in [Firmware updates](firmware-update).

## How it works

In provisioning mode, the stock firmware exposes a local configuration service. Its OTA endpoint has a stack-overflow vulnerability. A specially crafted request (payload) from the computer redirects execution into the firmware's own **factory OTA update process**.

TC1 then joins the open Wi-Fi network `PHICOMM_OTA_AP` and downloads `mico_ota.bin` from the TFTP server at `10.0.0.2`. It verifies the file, switches firmware slots and reboots into community firmware.

There are two connections: **the computer first joins the TC1 provisioning AP to send the trigger; TC1 then joins the router's AP to download the firmware.** Prepare the OTA AP and file server before sending the trigger. The connections do not need to share a Wi-Fi adapter.

## Prepare for flashing

### Equipment and tools

- A **TC1 A1 hardware power strip** running stock firmware **1.0.0.26**, **1.0.0.24** or **1.0.0.21**.
- A computer with **Ethernet and Wi-Fi adapters**, and a router that supports **2.4 GHz open Wi-Fi**.
- An Ethernet cable, `curl` and **tftpd or another TFTP server**. Python 3 is needed for custom image packaging or the scripts below.
- The community application-slot image `tc1_community.image.bin` for this hardware.

Review [power and wiring requirements](flash-prep) and [firmware package selection](firmware-update). Flashing erases the inactive application slot and reboots the device. Use [wired recovery](tc1-flash) if installation fails.

### Prepare the update file

Download the prepared [mico_ota.bin](assets/tools/tc1-no-disassembly/mico_ota.bin) (649344 bytes, **0.9.6-ed4474 migration image**) to skip packaging. Use the following method only when preparing another image; the fixed sizes and MD5 below apply only to the bundled snapshot.

The update file is the raw **MRVL application-slot image plus its 16-byte binary MD5**, not a 32-character text digest. The community image is 649328 bytes; the steps below produce a 649344-byte `mico_ota.bin`:

```sh
python3 - <<'PY'
import hashlib
image = open('tc1_community.image.bin', 'rb').read()
assert image.startswith(b'MRVL'), 'raw MRVL application-slot image required'
assert len(image) + 16 <= 0xA9000, 'image must fit the 692224 B slot'
open('mico_ota.bin', 'wb').write(image + hashlib.md5(image).digest())
print(len(image), hashlib.md5(image).hexdigest())
PY
```

The image MD5 should be `137bbe25d40f0a45b2b3129f035ea4c3`. Do not use a full-flash dump, a complete `.all.bin` or a `PHOTA001` web package as input; these checks only cover the MRVL marker, size and MD5, not hardware suitability.

## Network topologies

The procedure needs two connections with different roles, and they need not be on the same device:

| Role | What it does |
| --- | --- |
| Provisioning client | Joins the TC1 `Phicomm_TC1_XXXX` hotspot, gets `10.10.10.x/24`, sends the trigger to `10.10.10.1:8000` |
| OTA network | Provides the open 2.4 GHz hotspot `PHICOMM_OTA_AP` and lets the device reach the TFTP server at `10.0.0.2/8` |

Two computers, a computer plus a travel router, or a single device supporting concurrent STA/AP all work. TFTP can run on the AP itself or on a wired host bridged to the hotspot. When one radio serves both STA and AP, bring up `PHICOMM_OTA_AP` first and join the TC1 provisioning hotspot second: in the reverse order some drivers fail to start the AP.

### Serving TFTP with Python

Without a TFTP package, start the temporary service from step 4 of the [illustrated guide](tc1-no-disassembly-ethernet-wifi-ap). Listening on port 69 normally needs elevated privileges on Linux and macOS; on Windows allow UDP on that adapter when the firewall prompts.

### Fixed device-side parameters

| Parameter | Value |
| --- | --- |
| Provisioning hotspot / address | `Phicomm_TC1_XXXX`, `10.10.10.1/24`, configuration service on `:8000` |
| OTA hotspot | `PHICOMM_OTA_AP`, 2.4 GHz, open |
| TFTP service | `10.0.0.2:69`, octet, 512-byte blocks |
| Filename | `mico_ota.bin`, must not be renamed |
| Device OTA address | `10.<last three MAC bytes, each in decimal>`, netmask `/8` |
| Non-active slot capacity | `0xA9000` = 692224 B |

After joining the OTA hotspot the device erases the non-active slot (about 6.5 seconds on the sample unit), downloads from `10.0.0.2`, verifies the MD5 and switches slots to reboot. A successful first boot shows the open hotspot `TC1_A1-AP-<last six MAC characters, uppercase>` with the device at `192.168.0.1`.

### The trigger request

The trigger file is 397 bytes: 392 bytes of filler, the target address as 4 little-endian bytes, and a trailing NUL. The target address follows the `VERSION` returned by `/read-domain`:

| Stock version | Target address (little-endian) | Note |
| --- | --- | --- |
| 1.0.0.26 | `0x1F073D9F` (`9f 3d 07 1f`) | Factory OTA thread body |
| 1.0.0.24 | `0x1F073D63` (`63 3d 07 1f`) | Same thread body, address shifted by -0x3C |
| 1.0.0.21 | `0x1F073D2F` (`2f 3d 07 1f`) | Same thread body, address shifted by -0x34 |

Send it with the bundled script, which reads the version, picks the address and needs no arguments:

```sh
python3 tc1-trigger.py
```

If it cannot reach the device, or the version is not in the table above, it exits without falling back to another version's address. Send once; when the result is unconfirmed, watch the TFTP log and the device instead of resending.

## Other stock versions

The three verified versions share one frame layout (`BUF_OFF=0x148`, `LR_OFF=0x2d4`, `filler=392`); only the `tftp_ota` thread body address differs, which is where the three addresses above come from.

Other versions and other builds cannot reuse these addresses. Adapting a new version means obtaining that version's application-slot image first (a full SWD read, or a stock OTA package) and re-locating the vulnerable function and `tftp_ota`; guessing address offsets does not work. Writing the image directly over J-Link is independent of the stock version, see [wired installation and recovery](tc1-flash).

## Common problems

| Symptom | Likely cause |
| --- | --- |
| No `Phicomm_TC1_XXXX` | Wrong button rhythm, or not A1 hardware / not 1.0.0.26, 1.0.0.24 or 1.0.0.21 |
| `/OTA` returns 404 or hangs | Not in provisioning mode, or client outside `10.10.10.x/24` |
| Device never joins the OTA AP | Not 2.4 GHz, wrong SSID, or client isolation enabled |
| TFTP stalls after the first block | Firewall allows only UDP 69, not the transfer session |
| TFTP completes but no slot switch | Image MD5 mismatch, or image larger than 692224 B |

This page only covers the devices and version listed above. Other stock versions are not supported.
