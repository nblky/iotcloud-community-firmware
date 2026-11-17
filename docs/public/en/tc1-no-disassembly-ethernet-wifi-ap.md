# TC1 illustrated OTA guide

The computer serves the firmware over **Ethernet** and sends the trigger over **Wi-Fi**. The community image goes into the non-active slot, so the stock slot stays untouched. No case opening, no soldering.

**For TC1 A1 hardware (EMW3031 / MK3031), stock firmware 1.0.0.26, 1.0.0.24 or 1.0.0.21.** TC1 A2, other stock versions and the A1 air purifier are outside this procedure.

<!-- os-picker -->

## 1. Create the tc1-flash folder

Keep every file in one folder and run every command below from inside it (on Windows, use CMD or PowerShell):

```sh
mkdir tc1-flash
cd tc1-flash
```

Download these two files and keep their names:

| Download | Purpose |
| --- | --- |
| [mico_ota.bin](assets/tools/tc1-no-disassembly/mico_ota.bin) | 649,344-byte 0.9.6-ed4474 migration image; goes into the TFTP directory, filename must not change |
| [tc1-trigger.py](assets/tools/tc1-no-disassembly/tc1-trigger.py) | Reads the stock version and sends the trigger; keep it beside `mico_ota.bin` |

Check both files against [SHA256SUMS](assets/SHA256SUMS) and download again on a mismatch. Keep the power strip on stable power and unplug any load from its outlets.

Done when: `tc1-flash` holds both files with unchanged names.

## 2. Create the PHICOMM_OTA_AP hotspot, then connect Ethernet

TC1 leaves its provisioning hotspot as soon as it receives the trigger, then joins `PHICOMM_OTA_AP` to download the firmware. **Set up the download network before touching TC1.**

In the router or AP wireless settings, select **2.4 GHz** and confirm all four items:

| Setting | Value |
| --- | --- |
| Wi-Fi name / SSID | `PHICOMM_OTA_AP`, same case, no spaces |
| Security / password | Open, no password |
| Hide SSID | Off |
| Guest mode / client isolation | Off |

![Router wireless settings: PHICOMM_OTA_AP, no password, no hiding, no guest mode](assets/images/tc1-ota-ap.png)

Connect the computer to a **LAN port** of that router with an Ethernet cable. The router needs no internet access and its management address does not become `10.0.0.2` — that address belongs to the computer — but make sure nothing else on the LAN already uses it. Do not bridge Ethernet and Wi-Fi and do not enable connection sharing. The hotspot needs no DHCP: TC1 uses a static address of its own.

Done when: a phone joins the open `PHICOMM_OTA_AP` and the LAN port LED is lit.

## 3. Add 10.0.0.2 to the Ethernet adapter, and write down the old settings

The TFTP server address must be `10.0.0.2/8`, **with no gateway and no DNS**. Write the old settings down first; section 7 restores from this list:

```text
Ethernet adapter name:
Old address / netmask:
Old gateway:
Old DNS:
Was it DHCP or static:
```

<!-- os:windows -->

List the adapters and their current settings, then find the **Ethernet** adapter wired to the LAN port:

```cmd
netsh interface ipv4 show interfaces
ipconfig /all
```

The examples use `Ethernet`; substitute your adapter name inside the quotes:

```cmd
netsh interface ipv4 add address name="Ethernet" address=10.0.0.2 mask=255.0.0.0
netsh interface ipv4 show addresses name="Ethernet"
```

![Administrator command line adding 10.0.0.2/8 to Ethernet](assets/images/tc1-ota-windows-address.png)

<!-- /os -->

<!-- os:linux -->

Find the adapter name; the examples use `enp3s0`:

```sh
ip -br address
ip route
sudo ip address add 10.0.0.2/8 dev enp3s0
ip -4 address show dev enp3s0
```

<!-- /os -->

<!-- os:mac -->

Identify the Ethernet adapter first; a USB adapter is not necessarily `en0`. The examples assume `en5`:

```sh
networksetup -listallhardwareports
ifconfig en5
sudo ifconfig en5 inet 10.0.0.2 netmask 255.0.0.0 alias
ifconfig en5
```

<!-- /os -->

Done when: the Ethernet adapter actually shows `10.0.0.2/8`.

## 4. Start TFTP, then download once from another device

Put only `mico_ota.bin` in the TFTP directory; **do not put the payload there**. Keep the service running until the transfer finishes.

<!-- os:windows -->

Open Tftpd64 and set four things:

1. **Current Directory** → **Browse** → pick the `tc1-flash` folder.
2. **Server interfaces** → **`10.0.0.2`**, not the Wi-Fi address.
3. Open the **Tftp Server** tab and leave the window open to watch transfers; its DHCP service is not needed.
4. Allow Tftpd64 through Windows Firewall on this flashing network when prompted.

![Tftpd64: firmware directory selected, Server interfaces set to 10.0.0.2](assets/images/tc1-ota-tftpd64.png)

<!-- /os -->

<!-- os:linux -->

On Debian / Ubuntu install `dnsmasq-base`, then run this inside `tc1-flash`:

```sh
sudo apt install dnsmasq-base
tc1_tftp_dir=$(mktemp -d /tmp/tc1-tftp.XXXXXX)
chmod 755 "$tc1_tftp_dir"
install -m 644 mico_ota.bin "$tc1_tftp_dir/mico_ota.bin"
sudo dnsmasq --no-daemon --conf-file=/dev/null --port=0 \
  --listen-address=10.0.0.2 --bind-interfaces --enable-tftp \
  --tftp-root="$tc1_tftp_dir" --log-facility=-
```

This serves TFTP only, no DNS or DHCP. Leave the terminal running and send the trigger from a second terminal. If UDP 69 is already taken, use the existing service and its directory instead of starting another.

<!-- /os -->

<!-- os:mac -->

Run this inside `tc1-flash`. First make sure `/private/tftpboot` holds no same-named file you need to keep:

```sh
sudo mkdir -p /private/tftpboot
sudo cp mico_ota.bin /private/tftpboot/mico_ota.bin
sudo chmod 644 /private/tftpboot/mico_ota.bin
sudo launchctl load -w /System/Library/LaunchDaemons/tftp.plist
```

The built-in service has no progress window; watch the device in section 6 instead.

<!-- /os -->

TFTP requests start on **UDP 69**, but the transfer continues on a session port the server opens. The firewall must allow the whole transfer, not just UDP 69.

### Self-check: download once from another device

Before putting TC1 into provisioning mode, download the file completely from another device joined to `PHICOMM_OTA_AP`:

<!-- os:windows -->

```cmd
curl.exe --fail --max-time 30 -o tc1-download-check.bin tftp://10.0.0.2/mico_ota.bin
```

<!-- /os -->

<!-- os:mac linux -->

```sh
curl --fail --max-time 30 -o tc1-download-check.bin tftp://10.0.0.2/mico_ota.bin
```

<!-- /os -->

If curl lacks `tftp://`, use any TFTP client in binary / octet mode.

Done when: the download succeeds and the file is 649,344 bytes. Do not continue until this passes.

## 5. Put TC1 into provisioning mode and read the stock version

Power-cycle TC1 and **double-press the pairing button within 3 seconds** of powering on. After the LED starts blinking slowly, an open hotspot `Phicomm_TC1_XXXX` appears; the last four characters differ per device.

If no hotspot appears: hold the pairing button until the LED blinks fast, release, then short-press twice in quick succession. If it still does not appear, power-cycle and retry — do not send the trigger early.

Join that hotspot with the computer's **Wi-Fi** adapter; leave Ethernet and TFTP untouched. When Windows reports "No internet", stay connected instead of reconnecting.

![Stock provisioning hotspot in the Windows wireless list](assets/images/tc1-stock-ap.png)

Confirm Wi-Fi holds a `10.10.10.x/24` address, then read the stock identity:

```sh
curl --noproxy '*' --fail --max-time 5 http://10.10.10.1:8000/read-domain
```

On Windows use `curl.exe`. Never drop `--noproxy '*'`: a system proxy stops requests to `10.10.10.1` from reaching the device.

![read-domain returning the stock VERSION](assets/images/tc1-stock-version.png)

Read only `VERSION` from the response. It must be one of `1.0.0.26`, `1.0.0.24`, `1.0.0.21`. Anything else: stop and see [OTA internals and custom images](tc1-no-disassembly).

Done when: the JSON contains `VERSION` and it is one of those three values.

## 6. Run the script, then leave everything alone

Before sending, confirm three things: TFTP is still running, Ethernet still has `10.0.0.2`, and Wi-Fi is still on TC1. Then run this once:

```sh
python3 tc1-trigger.py
```

The script reads `VERSION` from `10.10.10.1:8000`, picks the target address for that version and sends the 397-byte trigger. If the version is outside the verified set, or the device cannot be reached, it exits without falling back to another version's address.

**Send once.** If the script reports the device answered, stop. If it reports the result unconfirmed, watch the TFTP log and the device instead of sending again.

### Manual path without Python

Download the file matching the version read in section 5: [tftp-trigger.bin](assets/tools/tc1-no-disassembly/tftp-trigger.bin) for 1.0.0.26, [tftp-trigger-1.0.0.24.bin](assets/tools/tc1-no-disassembly/tftp-trigger-1.0.0.24.bin) for 1.0.0.24, [tftp-trigger-1.0.0.21.bin](assets/tools/tc1-no-disassembly/tftp-trigger-1.0.0.21.bin) for 1.0.0.21.

Send it once from inside `tc1-flash`; on Windows use `curl.exe`:

```sh
curl --noproxy '*' --max-time 30 --header 'Content-Type: application/json' --data-binary '@tftp-trigger.bin' http://10.10.10.1:8000/OTA
```

`--data-binary` sends the file verbatim: do not open the payload and copy it as text, and do not paste prompts or backticks from a screenshot into the command.

![Stock endpoint returning OTA text after the curl trigger](assets/images/tc1-ota-trigger.png)

### The next one to two minutes

The device erases the non-active slot (about 6 seconds), downloads `mico_ota.bin` from `10.0.0.2:69`, verifies the trailing MD5, then switches slots and reboots. During this:

- the provisioning hotspot disappearing, Wi-Fi dropping and curl timing out are all expected;
- any TFTP log or progress window should show the read request and the complete transfer;
- the computer's Wi-Fi does not switch to `PHICOMM_OTA_AP`; Ethernet carries the transfer.

**Keep TC1, the AP and the computer powered; do not stop TFTP, unplug the cable or cut power while the download is running.**

Done when: the TFTP log shows `mico_ota.bin` transferred and the device reboots on its own.

## 7. Confirm the flash, then restore the network

A new open hotspot **`TC1_A1-AP-<last six MAC characters, uppercase>`** appears; `TC1_A1-AP-636D3D` in the screenshot is only an example.

![TC1_A1-AP hotspot after booting the community firmware](assets/images/tc1-community-ap.png)

Join it and open **`http://192.168.0.1/`**. Reaching the community firmware page means the flash worked. Set up your daily Wi-Fi there; buttons, provisioning and the six outlets are covered in [TC1 features and operation](tc1-guide).

Stop the temporary service and remove the temporary address:

<!-- os:windows -->

```cmd
netsh interface ipv4 delete address name="Ethernet" address=10.0.0.2
```

Close Tftpd64. If the adapter used DHCP before, restore it:

```cmd
netsh interface ipv4 set address name="Ethernet" source=dhcp
```

<!-- /os -->

<!-- os:linux -->

```sh
sudo ip address del 10.0.0.2/8 dev enp3s0
```

Press `Ctrl+C` in the terminal running dnsmasq.

<!-- /os -->

<!-- os:mac -->

```sh
sudo launchctl unload -w /System/Library/LaunchDaemons/tftp.plist
sudo ifconfig en5 inet 10.0.0.2 -alias
```

<!-- /os -->

**If the adapter was static before, do not run the DHCP command.** Fill the five entries recorded in section 3 back in: address and netmask, gateway, DNS, DHCP or static. Then turn off the temporary hotspot, or restore the router's original Wi-Fi name and password, and withdraw the firewall allowance you added.

For later upgrades take the latest compatible stable release from [Downloads]({{SITE_URL}}en/#releases); see [Firmware updates](firmware-update). No second trigger is needed.

Done when: the computer browses normally and both the router page and `http://192.168.0.1/` open.

If flashing fails, or the device stays in provisioning mode without booting, follow [wired installation and recovery](tc1-flash). The stock slot was not modified, so recovery is possible.

## Common questions

| Symptom | Do this one thing first |
| --- | --- |
| Script cannot reach `10.10.10.1:8000` | Back to section 5: is Wi-Fi still on `Phicomm_TC1_XXXX` and does it hold `10.10.10.x`? |
| Manual curl cannot find the payload | Run it inside `tc1-flash`, make sure the name is not `tftp-trigger.bin.txt`, and that the file matches the version read in section 5 |
| TFTP sees no request | Check the four hotspot settings in section 2, the service binding and firewall in section 4 |
| TFTP stalls mid-transfer | Check whether the session port is blocked, the computer slept, or the cable is loose |
| No new hotspot after the transfer | Wait one to two minutes and check the TFTP log for a completed `mico_ota.bin` |

Network topologies, custom images and internals: [OTA internals and custom images](tc1-no-disassembly). Downloader-based recovery: [wired installation and recovery](tc1-flash).
