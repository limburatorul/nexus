<p align="center"><img src="res/bridger.ico" width="96" height="96" alt=""></p>
<h1 align="center">Bridger</h1>
<p align="center">O tastatură, un mouse și sunetul, pentru mai multe PC-uri din rețea, de la Windows XP la Windows 11.</p>

Împingi cursorul peste marginea ecranului și el continuă pe celălalt PC. Tastatura merge după el,
clipboard-ul și fișierele copiate te urmează, iar sunetul PC-ului controlat se aude pe boxele
PC-ului principal.

> **Stare:** în lucru, etapa M0 (fereastra, tray-ul și installerul). Planul complet e în [SPEC.md](SPEC.md).

## Sisteme suportate

| Rol | Windows |
|---|---|
| PC principal (are tastatura și mouse-ul) | 7, 8, 10, 11 (XP merge și el) |
| PC controlat | XP SP3, Vista, 7, 8, 10, 11 |

Același `Bridger-Setup-<versiune>.exe` merge pe toate. Pe XP, sunetul se captează prin intrarea
„Stereo Mix” / „What U Hear” a plăcii de sunet, dacă driverul o are.

## Build

1. [WinLibs](https://winlibs.com) MinGW-w64 **i686**, cu runtime **msvcrt** (nu UCRT). Îl pui în
   `%USERPROFILE%\tools\mingw32` sau setezi variabila `MINGW` spre acel folder.
2. [NSIS 3](https://nsis.sourceforge.io).
3. Python 3, folosit de verificarea importurilor.
4. Rulezi `build.bat`. Rezultatul ajunge în `build\bridger.exe` și `build\Bridger-Setup-<versiune>.exe`.

`build.bat` se oprește dacă exe-ul importă o funcție nevăzută pe XP; lista celor aprobate e
`tools/xp_imports.txt`.

## Mulțumiri

Aspectul (paleta „Vernier”, structura cu sidebar, module și footer) e preluat din
[Beamer](https://github.com/kalkman-code/beamer) de Toby George Read Kalkman, GPL-3.0. Ideile de
funcționalitate vin din Beamer și din Stardock Multiplicity. Fonturile sunt Hanken Grotesk și
B612 Mono, sub licența SIL Open Font License (`res/fonts`).

## Licență

GPL-3.0-or-later, vezi [LICENSE](LICENSE).
