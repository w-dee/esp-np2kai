# ESP32-P4-NANO USB Host Stage 1

独立したESP-IDF USB Host/HIDキーボード診断です。現在の範囲は、
Waveshare ESP32-P4-NANO Type-AへUSB HIDキーボードを直接接続する
Stage 1だけです。外部ハブ、Hub Driver、TT、マウス、ストレージ、
製品ファームウェア統合はこの診断に含めません。

## Build boundary

- Board: Waveshare ESP32-P4-NANO
- Target: ESP32-P4 rev v1.3 (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`)
- ESP-IDF: v5.5.4
- USB Host: ESP-IDF built-in USB Host Library
- USB peripheral: default High-Speed Host instance (`peripheral_map = 0`)
- HID component: `espressif/usb_host_hid` exactly `1.0.3`
- Hub support: disabled
- PSRAM-backed USB DMA: disabled
- Unrelated peripherals: not initialized

P4のHS OTG PHYは専用のUSB_DM/USB_DPパッケージピンを使用します。FS OTGの
GPIO26/27やUSB Serial/JTAGのGPIO24/25を、この診断のType-A HS経路として
扱いません。

## Type-A VBUS

P4-NANOのType-A VBUSは、基板上のDIO7003HEST5を通じて`VCC_5V`から
`VBUS_OUT`へ供給されます。VBUSは診断のGPIOで制御しません。過電流フラグも
P4ソフトウェアから利用できません。ロードスイッチの部品定格や基板上の
`VCC_5V`表記から、USBシステムの保証電流値は主張しません。

Stage 1では、電源投入前から通常のUSBキーボードを1台だけType-Aへ接続します。

## Direct keyboard acceptance

列挙後、Boot Protocolへ切り替え、以下を正規化した生HID usage列で確認します。

```text
A, 1, Space, Enter,
Left Shift + A,
Left Ctrl press/release,
Left Alt press/release,
F1, Up, Down, Left, Right
```

8-byte Boot Protocolレポートはプロジェクト所有の純粋なパーサで処理します。
通常キーのmake/break、8個のmodifier、同時押し、キー置換、ErrorRollOver/
POSTFail/ErrorUndefinedを扱います。ASCIIは補助表示に限定し、受入証拠には
使用しません。

列挙タイムアウトは20秒、キーボード有効化後のキー列確認は120秒です。
タイムアウトや不正レポートで自動再起動は行いません。

ソフトウェアの最終マーカーは次です。

```text
P4-NANO USB HOST START
P4-NANO USB HOST LIB INIT: PASS/FAIL
P4-NANO USB ROOT PERIPHERAL: HS
P4-NANO USB ROOT RESULT: PASS/FAIL
P4-NANO USB DIRECT DEVICE: PASS/FAIL
P4-NANO USB DIRECT VIDPID: xxxx:xxxx
P4-NANO USB DIRECT SPEED: LS/FS/HS/UNKNOWN
P4-NANO USB DIRECT HID: PASS/FAIL
P4-NANO USB DIRECT INPUT: PASS/FAIL
P4-NANO USB DIRECT DIGITAL RESULT: PASS
P4-NANO USB DIRECT RESULT: PASS/FAIL
P4-NANO USB HOST RESULT: PASS/FAIL
```

`P4-NANO USB PHYSICAL RESULT: PASS`は自動出力しません。人間が実際に押した
キー列を確認した後にのみ、物理結果を別途記録します。

## Stage 2 deferred

外部ハブの試験は、直接キーボードのStage 1がPASSした後に別タスクで扱います。
HSハブ配下のFS/LS子機はESP-IDF v5.5.4のTT制限対象です。FS-onlyハブの
実挙動も未検証です。このREADMEではHub PASSやUSB via Hub PASSを主張しません。

## Native parser test

ESP-IDFを必要としない決定的テストを実行します。

```bash
cc -std=c11 -Wall -Wextra -Werror \
  main/hid_boot_keyboard.c native/test_hid_boot_keyboard.c \
  -o /tmp/p4-nano-usb-host-hid-test
/tmp/p4-nano-usb-host-hid-test
```

## Physical flash boundary

通常のP4診断フラッシュのみを使用します。全消去、`--force`、eFuse変更、
USB PHY経路変更、C6フラッシュは行いません。Stage 1の物理試験では外部ハブを
接続せず、Type-C/CH343P UART経路だけを通常の書き込み・ログ取得に使用します。
