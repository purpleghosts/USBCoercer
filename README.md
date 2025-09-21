| Supported Targets | ESP32-S3 |
| ----------------- | -------- |

# USBCoercer: A TinyUSB based WPAD Coercion Device

USBCoercer turns an ESP32 development board with native USB-OTG into an Ethernet-over-USB gadget capable of coercing proxy configuration via WPAD. It builds on the **TinyUSB Network Control Model (NCM)** example and adds a minimalist DHCP server that injects DHCP option 252 (WPAD/PAC) and, additionally, classless static routes (option 121) to block EDR telemetry if needed.

The project is intended for security testing and lab demonstrations. Always obtain explicit authorization before using it.

## Key Features

- TinyUSB-based USB NCM Ethernet gadget
- Embedded DHCP server providing optional classless static routes (option 121) and configurable WPAD/PAC URL (option 252).
- Status LED indicator that turns green after the network stack is up on AtomS3U hardware, yellow after DHCP Discovery/Offer phase and blue when DHCP Request/ACK is done.

## Requirements

### Hardware

- ESP32-S3 board with native USB-OTG
- (In some models) USB cable connecting the SoC OTG port to the target machine.
  
### Software

- Espressif ESP-IDF v5.5 (or newer) with TinyUSB support enabled.
- ESP-IDF toolchain and Python environment installed via the standard `install.sh`/`export.sh` scripts.
- **Important:** This firmware has been developed with the 1.x branch of espressif/esp_tinyusb and has not been ported to the 2.x branch. Using the 2.x branch will result in compilation errors. The maximum supported version is [espressif/esp_tinyusb 1.6.0](https://components.espressif.com/components/espressif/esp_tinyusb/versions/1.6.0/readme).

## Quick Start

1. **Set up ESP-IDF**
   ```bash
   . $HOME/esp/esp-idf/export.sh
   ```
2. **Configure the project (optional)**
   ```bash
   idf.py menuconfig
   ```
   Review the `USBCoercer Configuration` menu to tailor the local IP, DHCP pool, WPAD settings, etc.
3. **Build**
   ```bash
   idf.py build
   ```
4. **Flash**
   ```bash
   idf.py flash
   ```

### Configuration Reference

The project exposes a dedicated `USBCoercer Configuration` menu inside
`idf.py menuconfig`:

- **Interface** – set the USB-facing IPv4 address, subnet mask and gateway as
  well as the locally administered MAC address advertised to the host.
- **DHCP server** – control the lease pool start/end, lease time, optional DNS
  server and domain suffix (option 15).
- **WPAD** – toggle option 252 and define the URL served to connected hosts.
- **Static routes** – enable option 121 and provide a semicolon-separated list
  of routes using the `destination/prefix,gateway` syntax (for example,
  `3.121.6.180/32,192.168.7.1`). Leave the list empty to omit the option.

### Default Configuration

`sdkconfig.defaults` provides a lab-friendly setup:

- Local IP: `192.168.7.1/24` with no default gateway.
- DHCP pool of three addresses (`192.168.7.2` – `192.168.7.4`).
- DHCP domain: `badnet`.
- WPAD enabled, pointing to `http://192.168.7.1/wpad.dat`. Host a PAC file on
  the configured origin or adapt the URL as required.
- DNS option disabled (hosts retain their existing DNS servers).
- Static routes disabled by default (option 121 is only sent when configured).

Adjust `sdkconfig.defaults` or use `menuconfig` to adapt the environment to your tests. Rebuild the firmware (`idf.py build`) after changing any parameters.

## Network Behaviour

- The device exposes itself as a USB NCM Ethernet adapter using the configured MAC address.
- The embedded DHCP server answers `DISCOVER`/`REQUEST` messages with the configured parameters.
- When classless routes are enabled, option 121 is generated dynamically.
- WPAD is enabled and a non-empty URL is provided, so the host retrieves the PAC file from the specified origin and applies the proxy settings.

## Using an NTLM Coercion Mechanism

Currently, the deployment requires an **external server** to provide the NTLM-authenticated proxy and coerce authentication. [Responder](https://github.com/SpiderLabs/Responder) is useful for this job. An **external server** to serve the WPAD.DAT file is optional.

## Warnings

- Manipulating WPAD or static routes can redirect traffic from the connected machine. Use this firmware only in controlled environments with informed consent.
- The project targets lab scenarios; it does not implement additional security controls or configuration persistence.

## Testing

This firmware has been tested on [M5Stack AtomS3U](https://shop.m5stack.com/products/atoms3u) and worked properly. 

## Acknowledgements

- Based more or less closely, or distantly, on the official ESP-IDF TinyUSB NCM examples.
- Refactored and improved with [Codex](https://openai.com/index/introducing-codex/).

Happy (and responsible) hacking!
