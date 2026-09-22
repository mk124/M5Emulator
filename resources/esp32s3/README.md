# ESP32-S3 Bootloader

`bootloader.bin` is an ESP32-S3 second-stage bootloader built with ESP-IDF v6.0.2.

Used when loading standalone application BINs. Full flash images use their own bootloader. The partition table offset is `0x8000`, and application rollback is enabled.

See [LICENSE](LICENSE) for Apache-2.0, Newlib, and libgcc license terms and notices, including the GCC Runtime Library Exception.
