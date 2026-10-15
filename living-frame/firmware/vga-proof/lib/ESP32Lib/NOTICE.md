# NOTICE — vendored third-party library

`src/` in this directory is a copy of **bitluni's ESP32Lib**, taken from
<https://github.com/bitluni/ESP32Lib> at commit
`35b17d26f3ed03962b98fb8d0693a994d01bbc70` (branch `master`, `library.json`
declares version 0.4.0), fetched 2026-10-15.

Upstream is licensed **Creative Commons Attribution-ShareAlike 4.0**
(<https://creativecommons.org/licenses/by-sa/4.0/>), author bitluni, with the
0.4.0 refactor by Martin Laclaustra. This copy is redistributed under the same
licence; no upstream file other than `src/I2S/I2S_ESP32.cpp` was modified.

## The one modification, in full

Upstream calls the ESP-IDF 3.x RTC clock API, which no longer exists in the
ESP-IDF 4.4 that ships inside arduino-esp32 2.x:

```c
rtc_clk_apll_enable(true);
rtc_clk_apll_coeff_set(odir, sdm & 255, (sdm >> 8) & 255, sdm >> 16);
```

The current API takes the divisors as arguments, and the ESP-IDF 3.x call it
replaces is already present as a comment on the line above it upstream:

```c
rtc_clk_apll_enable(true, sdm & 255, (sdm >> 8) & 255, sdm >> 16, odir);
```

That substitution was applied at both call sites in
`src/I2S/I2S_ESP32.cpp` (originally around lines 378-380 and 461-462; now
378 and 458). Without it the build fails with:

```
I2S_ESP32.cpp:379:27: error: too few arguments to function
  'void rtc_clk_apll_enable(bool, uint32_t, uint32_t, uint32_t, uint32_t)'
I2S_ESP32.cpp:380:3: error: 'rtc_clk_apll_coeff_set' was not declared in this scope
```

Nothing else differs from upstream. Upstream has the same comment at the
ESP8266 variant, which is untouched (it does not build for ESP32).

To re-derive this copy:

```bash
git clone https://github.com/bitluni/ESP32Lib
git -C ESP32Lib checkout 35b17d26f3ed03962b98fb8d0693a994d01bbc70
cp -r ESP32Lib/src lib/ESP32Lib/src
# then apply the substitution above twice in lib/ESP32Lib/src/I2S/I2S_ESP32.cpp
```
