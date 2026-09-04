# Example NEXUS user config

A worked `zmk-config` showing how NEXUS plugs into a keyboard you already have.
Copy the pieces you need into your own config repo.

```
config/
├── west.yml          # pulls ZMK + the NEXUS module
├── nexus.conf        # every NEXUS knob, commented
├── nexus/
│   └── splash/       # drop splash.png here
└── boards/shields/   # your keyboard's shields stay yours
build.yaml            # one artifact per role
```

## The three-line version

1. Add NEXUS to `config/west.yml`:

   ```yaml
   - name: nexus
     remote: nexus        # url-base: https://github.com/vaibhav8600-rgb
     revision: v1.0.0
   ```

2. Add `nexus_dongle` next to your dongle shield in `build.yaml`:

   ```yaml
   shield: nexus_dongle sofle_dongle
   snippet: studio-rpc-usb-uart
   ```

3. Append `nexus.conf` to your dongle's `.conf`.

That is the whole integration. No NEXUS source is edited, ever.

## What builds out of the box

Only the `nexus_standalone` entry -- it uses the shipped demo shield and needs
no keyboard. The other entries expect your own Sofle shields; see
`config/boards/shields/README.md`.
