<p align="center"><img src="res/nexus.ico" width="96" height="96" alt=""></p>
<h1 align="center">Nexus</h1>
<p align="center">O tastatură, un mouse și sunetul, pentru mai multe PC-uri din rețea, de la Windows XP la Windows 11.</p>

Împingi cursorul peste marginea ecranului și el continuă pe celălalt PC. Tastatura merge după el,
clipboard-ul și fișierele copiate te urmează, iar sunetul PC-ului controlat se aude pe boxele
PC-ului principal. Până la patru PC-uri controlate se aranjează prin tragere pe o hartă în jurul
PC-ului principal, iar cursorul trece din unul în altul ca între monitoarele aceluiași PC.

> **Stare:** în lucru, versiunea 0.5: mouse, tastatură, clipboard (text și imagini), sunet, fișiere, update automat, descoperire în rețea și mai multe PC-uri pe o hartă, pe Windows (XP–11) și Linux (Wayland). Testată automat între Windows 11, XP, Kubuntu și Manjaro; planul și stadiul sunt în [SPEC.md](SPEC.md).

## Ce e nou în 0.5

- **Marginea nu mai trece la atingere.** Cursorul trebuie să împingă puțin în ea (Touch / Light / Normal / Firm pe
  pagina Crossing), colțurile nu trec niciodată, iar imediat după sosire marginea ține o clipă. Butonul Close al
  unei ferestre maximizate rămâne la îndemână.
- **Monitoare de mărimi diferite:** o margine trece oriunde nu e alt monitor dincolo de ea, și cu DPI diferit pe
  fiecare monitor (Windows 10+).
- **Fără blocaje:** un buton „rămas apăsat” nu mai ține marginea închisă; o legătură căzută aduce inputul acasă în
  ~2 s; un PC controlat blocat (lock screen, UAC) trimite inputul înapoi și îți spune de ce.
- **Clipboard cu imagini** și text mare (până la 8 MB, respectiv 256 KB), inclusiv între Windows și Linux.
- **Tray:** iconița arată unde e inputul; meniul are Send input across, Pause crossing, Wake, Open log folder și
  About; notificări când inputul se întoarce singur. Opțional, marginile țin cât rulează un joc full-screen.
- **Cursorul PC-ului nefolosit dispare** și revine la loc la ieșire, chiar și după un crash.
- **Tasta de comutare se alege** (dublu-tap sau ținută), iar fiecare PC poate avea o tastă de salt direct.
- **Wake-on-LAN** pentru PC-urile controlate adormite.
- **Forward secrecy:** fiecare conexiune între două PC-uri 0.5 are și chei X25519 efemere, deci o parolă aflată mai
  târziu nu deschide o sesiune înregistrată. Cu un PC 0.4 legătura merge în continuare (și update-ul automat).
- **Log** în `%APPDATA%\Nexus\nexus.log` (Linux: `~/.config/nexus/nexus.log`) și `crash.log`; atașează-le la un
  bug report. Pagina de setări are acum scroll.

## Sisteme suportate

| Rol | Windows | Linux |
|---|---|---|
| PC principal (are tastatura și mouse-ul) | 7, 8, 10, 11 (XP merge și el) | KDE Plasma 6.1+ / GNOME 45+ pe Wayland |
| PC controlat | XP SP3, Vista, 7, 8, 10, 11 | idem |

Același `Nexus-Setup-<versiune>.exe` merge pe toate. Pe XP, sunetul se captează prin intrarea
„Stereo Mix” / „What U Hear” a plăcii de sunet, dacă driverul o are.

## Linux

Python 3.10+ cu PySide6, PyNaCl și jeepney, plus `libei` și `parec`/`pacat` (pachete de sistem):

```sh
python3 -m venv ~/nexus-venv && ~/nexus-venv/bin/pip install PySide6-Essentials pynacl jeepney
cd linux && ~/nexus-venv/bin/python -m nexus
```

La prima pornire desktopul cere permisiunea de a controla tastatura și mouse-ul.

## Build (Windows)

1. [WinLibs](https://winlibs.com) MinGW-w64 **i686**, cu runtime **msvcrt** (nu UCRT). Îl pui în
   `%USERPROFILE%\tools\mingw32` sau setezi variabila `MINGW` spre acel folder.
2. [NSIS 3](https://nsis.sourceforge.io).
3. Python 3, folosit de verificarea importurilor.
4. Rulezi `build.bat`. Rezultatul ajunge în `build\nexus.exe` și `build\Nexus-Setup-<versiune>.exe`.

`build.bat` se oprește dacă exe-ul importă o funcție nevăzută pe XP; lista celor aprobate e
`tools/xp_imports.txt`.

## Mulțumiri

Aspectul (paleta „Vernier”, structura cu sidebar, module și footer) e preluat din
[Beamer](https://github.com/kalkman-code/beamer) de Toby George Read Kalkman, GPL-3.0. Ideile de
funcționalitate vin din Beamer și din Stardock Multiplicity. Din 0.5, modelul de presiune al marginii (rezistență,
colțuri, grație), marginile pe monitoare decalate, detecția legăturii moarte, PC-ul blocat, ascunderea cursorului,
limitele clipboardului și forward secrecy sunt porturi din Beamer; fiecare fișier portat o spune într-un comentariu. Fonturile sunt Hanken Grotesk și
B612 Mono, sub licența SIL Open Font License (`res/fonts`).

## Licență

GPL-3.0-or-later, vezi [LICENSE](LICENSE).
