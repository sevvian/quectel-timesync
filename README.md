# Quectel Timesync

## About

This tool allows for acquiring the current time from the cellular network for configuring the local clock.
Compared to NTP, this has the advantage of not using up mobile traffic.

It takes advantage of the `AT+QLTS` command found on Quectel modems. This functionality depends on support
of the mobile network.


## Configuration

quectel-timesync supports UCI configuration.

## `enabled`

Determines if the tool should operate in daemon mode on system-start.

Valid values: `0` or `1`

## `device`

Path for the modem device to use.

In case this option is set, it overrides the `path` option.

Example: `/dev/cdc-wdm0`

## `path`

Path for the serial interface to use.

This option is overriden by the `device` option.

Example: `/dev/ttyUSB4`

## `interval`

Interval in seconds between syncronization attempts.

Minimum: `10`


## Supported Modems

 - Quectel EC25

Improvement for Luci app and making the timesync program detect vaarying date format and timezones

luci/
├── Makefile
├── htdocs
│   └── luci-static
│       └── resources
│           └── view
│               └── quectel-timesync
│                   ├── form.js
│                   └── status.js
├── po
│   └── templates
│       └── quectel-timesync.pot
└── root
    ├── etc
    │   ├── uci-defaults
    │   │   └── 80_quectel-timesync
    │   └── config
    │       └── quectel-timesync   (optional default)
    └── usr
        ├── libexec
        │   └── rpcd
        │       └── luci.quectel-timesync
        └── share
            ├── luci
            │   └── menu.d
            │       └── luci-app-quectel-timesync.json
            └── rpcd
                └── acl.d
                    └── luci-app-quectel-timesync.json
