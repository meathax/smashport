# ymfm (vendored)

- Source: MAME 0.289 tag, `3rdparty/ymfm/src` and `3rdparty/ymfm/LICENSE`
  (https://github.com/mamedev/mame/tree/mame0289/3rdparty/ymfm).
- License: BSD-3-Clause, copyright Aaron Giles (see `LICENSE`).
- Files are unmodified. Only the OPM (YM2151) engine is compiled.
- SHA-256: ymfm.h 947eab9c…bbe, ymfm_fm.h 1b27bfb7…5c9, ymfm_fm.ipp 4fb7fe59…4b9,
  ymfm_opm.h cab75025…ee7b, ymfm_opm.cpp 2235d571…22dc, LICENSE 2d2e9213…691c.
- Used so the YM2151 output, timers and busy flag match the MAME reference exactly (PLAN P6).
