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
- Cârligele rulează pe un fir propriu, ca un UI ocupat să nu blocheze inputul sistemului.
- Cursorul atinge marginea configurată (și niciun buton nu e apăsat) → modul „remote”: evenimentele locale
  sunt înghițite, cursorul e parcat în centrul ecranului sub o fereastră invizibilă fără cursor (deci dispare),
  iar deplasările (dx, dy) și tastele (virtual key + scancode + flaguri) pleacă pe rețea.
- Tastele ținute apăsate în momentul trecerii se eliberează local, nu pe celălalt PC.
- Revenirea: marginea opusă de pe PC-ul controlat (acesta raportează când cursorul lui o atinge),
  dublu-tap pe **Right Ctrl** sau, ca scăpare de urgență, **Ctrl+Alt+Pause**.
- Dacă legătura cade cât timp suntem în modul remote, controlul revine imediat local.

**Controlat (client):** `SendInput` absolut pentru mouse (poziția e urmărită local, accelerația se aplică o
singură dată, pe principal) și pe virtual key pentru taste. Când cursorul e la marginea de întoarcere și
mișcarea împinge mai departe, trimite LEAVE cu poziția de-a lungul marginii. La LEAVE sau la deconectare,
eliberează toate tastele și butoanele ținute. Pe un ecran scalat la 150%, Windows poziționează inputul absolut
pe o grilă de 1,5 px, deci precizia e ±1 px.

**Transport (implementat în `src/link.c`):** TCP pe portul 24800. Controlatul ascultă, principalul se conectează.
- Handshake în clar, 30 de octeți în fiecare sens: `"BRDG"`, versiunea legăturii (1), rolul (0 principal,
  1 controlat) și un nonce aleator de 24 de octeți.
- Cheia: `SHA-512(cheie_parolă ‖ nonce_principal ‖ nonce_controlat)`; primii 32 de octeți sunt pentru
  principal → controlat, următorii 32 pentru invers. `cheie_parolă` = SHA-512 iterat de 20.000 de ori peste
  `"bridger1" ‖ parolă`; în `bridger.ini` se salvează doar această cheie, niciodată parola.
- Cadre: `[lungime u32 LE][XSalsa20-Poly1305]`; nonce-ul e contorul de cadre al direcției și nu se trimite,
  deci un cadru reluat sau reordonat nu se deschide.
- Primul cadru în fiecare sens e `HELLO` cu numele PC-ului. Dacă nu se deschide, parolele diferă.
- Mesaje: `HELLO 1`, `PING 2`/`PONG 3` (u32 ms, la fiecare secundă; 6 s de liniște = legătură căzută),
  `ENTER 4` [margine, păstrează-poziția, poziție u16], `LEAVE 5` [are-poziție, poziție u16],
  `MOVE 6` [dx i16, dy i16], `BUTTON 7` [buton 1–5, apăsat], `WHEEL 8` [delta i16, orizontal], `KEY 9` [vk, scan, flaguri].
- Audio va pleca separat, pe UDP, ca să nu fie blocat de un transfer de fișiere.

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
2. **M1** ✅ (testat pe Win11 contra unor parteneri falși; **netestat încă pe XP real**) Rețea + criptare + mouse/tastatură: margine, dublu-tap, urgență, reconectare. Teste: `build/link_test.exe` (în build), `tests/fake_main.c`, `tests/fake_controlled.c`.
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
- Descoperirea automată în rețea: deocamdată adresa PC-ului controlat se scrie de mână.

## Limitări cunoscute
- Pe un PC controlat cu Vista sau mai nou, inputul nu ajunge în ferestrele pornite ca administrator (UIPI)
  cât timp Bridger rulează neelevat. Soluția: pornire elevată prin Task Scheduler la logare.
- Fără scroll în fereastra de setări: o pagină mai înaltă decât fereastra intră sub footer.
