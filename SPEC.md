# Bridger: KVM software (tastatură + mouse + sunet + clipboard + fișiere), XP → Win11

Inspirat din Beamer (github.com/kalkman-code/beamer, GPL-3: preluăm ideile și design-ul, de aceea și Bridger e GPL-3) și din Stardock Multiplicity.

## Obiectiv
Un PC **principal** (are tastatura și mouse-ul fizice, Win7–11) controlează unul sau mai multe PC-uri
**controlate** (Windows XP SP3 până la 11), în rețeaua locală. Sunetul PC-ului controlat se aude pe
boxele principalului.

## Decizii
| Ce | Alegere | De ce |
|---|---|---|
| Limbaj | C / Win32, un singur `bridger.exe` pentru ambele roluri | Singura variantă care merge pe XP fără runtime instalat |
| Compilator | MinGW-w64 **i686**, runtime **msvcrt** (nu UCRT), `_WIN32_WINNT=0x0501` | Binar 32-bit rulează pe XP 32-bit și pe Win11 64-bit |
| Criptare | TweetNaCl (1 fișier C, domeniu public): `crypto_secretbox` (XSalsa20-Poly1305) | Fără OpenSSL; merge pe XP |
| Cheie | parolă comună în `bridger.ini`; cheia de sesiune = SHA-512(parolă ‖ nonce_A ‖ nonce_B) | Nonce-uri noi la fiecare conexiune; o parolă slabă rămâne totuși ghicibilă offline, deci recomandăm una lungă |
| Config | `%APPDATA%\Bridgerridger.ini` (`GetPrivateProfileString`) | Nativ, ușor de editat de mână; Program Files nu e scriibil |
| Interfață | Fereastră Win32 desenată de mână, cu aspectul Beamer (paleta Vernier, Hanken Grotesk), și iconiță în tray | Qt-ul din Beamer nu rulează pe XP; un singur cod pentru XP, 7 și 10/11 |
| Installer | NSIS 3, un singur `Bridger-Setup-x.y.z.exe` | NSIS suportă XP, Inno Setup 6 nu |
| Repo | github.com/limburatorul/bridger, public, GPL-3.0 | Preluăm design-ul Beamer (GPL-3) |
| Autoupdate | Principalul verifică GitHub Releases și instalează silențios; **controlatul primește update-ul de la principal**, prin legătura criptată | XP nu are TLS 1.2, deci nu poate vorbi direct cu GitHub |

## Cum funcționează
**Principal (server):** cârlige `WH_MOUSE_LL` + `WH_KEYBOARD_LL` (există de la Win2000).
- Cursorul atinge marginea configurată → intrăm în modul „remote”: evenimentele locale sunt înghițite,
  cursorul e ținut în centru, iar deplasările relative (dx, dy) și tastele (scancode + flaguri) pleacă pe rețea.
- Revenirea: marginea opusă de pe PC-ul controlat (acesta raportează când cursorul lui o atinge),
  dublu-tap pe **Right Ctrl** sau, ca scăpare de urgență, **Ctrl+Alt+Pause**.
- Dacă legătura cade cât timp suntem în modul remote, controlul revine imediat local.

**Controlat (client):** `SendInput` pentru mouse și tastatură, plus raportarea atingerii marginii de întoarcere.

**Transport:** o conexiune TCP pentru control (input, clipboard, fișiere), cu cadre de forma
`[len u32][counter u32][secretbox(type u8 + payload)]`. Audio pleacă separat, pe UDP, ca să nu fie
blocat de un transfer de fișiere.

**Sunet (controlat → principal):**
- Vista și mai nou: WASAPI loopback, adică exact ce se aude pe ieșirea implicită.
- XP: `waveIn` de pe dispozitivul „Stereo Mix” / „What U Hear”. **Depinde de placa de sunet:** dacă
  driverul nu oferă acest dispozitiv, pe XP nu putem capta sunetul fără un driver virtual.
- Trimitem PCM 16-bit stereo la 44.1/48 kHz (~1.5 Mbit/s), fără codec, ceea ce e neglijabil în LAN.
  Principalul redă prin `waveOut`, cu un jitter buffer de ~60–100 ms.

**Clipboard text:** la fiecare comutare, partea care pierde focusul trimite `CF_UNICODETEXT`.

**Protocol:** e același pe toate versiunile de Windows și are un octet de versiune în handshake, deci principalul și controlatul trebuie doar să aibă aceeași versiune de protocol, nu aceeași versiune de Windows.

**Fișiere:** copiezi fișiere în Explorer (clipboard `CF_HDROP`) → la comutare sunt trimise în
`Desktop\KVM Primite\` pe celălalt PC, care le pune în clipboard ca `CF_HDROP`, deci **Ctrl+V** le
lipește unde vrei. Fișierele mai mari decât o limită din config nu se trimit automat.

## Etape (fiecare se testează înainte de următoarea)
1. **M0** ✅ Toolchain + schelet: `bridger.exe` cu fereastră și tray, instanță unică, installer NSIS, build XP-compatibil verificat (`tools/check_xp_imports.py`).
2. **M1** Rețea + criptare + mouse/tastatură: margine, dublu-tap, urgență, reconectare. *Test: XP ↔ Win11.*
3. **M2** Clipboard text.
4. **M3** Sunet (WASAPI pe Vista+, Stereo Mix pe XP).
5. **M4** Transfer fișiere prin copy/paste.
6. **M5** Autoupdate: GitHub Releases → principal → controlați (inclusiv XP), cu installerul rulat silențios (`/S`).
7. **M6** Extra din Multiplicity, doar dacă le vrei: mai multe PC-uri controlate, blocare/screensaver
   sincronizat, controlat → principal în ambele sensuri.

## Nu facem (cel puțin în v1)
- Ctrl+Alt+Del către PC-ul controlat: cere un serviciu Windows sau winlogon. Pentru asta folosim combinația de urgență.
- Ecranul de logon / UAC securizat pe controlat: același motiv.
- Efectele grafice din Beamer.
