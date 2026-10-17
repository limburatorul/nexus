# Nexus: KVM software (tastatură + mouse + sunet + clipboard + fișiere), XP → Win11

Inspirat din Beamer (github.com/kalkman-code/beamer, GPL-3: preluăm ideile și design-ul, de aceea și Nexus e GPL-3) și din Stardock Multiplicity.

## Obiectiv
Un PC **principal** (are tastatura și mouse-ul fizice, Win7–11) controlează unul sau mai multe PC-uri
**controlate** (Windows XP SP3 până la 11), în rețeaua locală. Sunetul PC-ului controlat se aude pe
boxele principalului.

## Decizii
| Ce | Alegere | De ce |
|---|---|---|
| Limbaj | C / Win32, un singur `nexus.exe` pentru ambele roluri | Singura variantă care merge pe XP fără runtime instalat |
| Compilator | MinGW-w64 **i686**, runtime **msvcrt** (nu UCRT), `_WIN32_WINNT=0x0501` | Binar 32-bit rulează pe XP 32-bit și pe Win11 64-bit |
| Criptare | TweetNaCl (1 fișier C, domeniu public): `crypto_secretbox` (XSalsa20-Poly1305), `crypto_scalarmult` (X25519) | Fără OpenSSL; merge pe XP |
| Cheie | parolă comună în `nexus.ini`; cheia de sesiune = SHA-512(parolă ‖ nonce_A ‖ nonce_B), iar din 0.5, între două PC-uri 0.5, amestecată cu un schimb X25519 efemer (forward secrecy) | Nonce-uri și chei efemere noi la fiecare conexiune; o parolă slabă rămâne totuși ghicibilă offline, deci recomandăm una lungă |
| Log | `%APPDATA%\Nexus\nexus.log` (1 MB, apoi `nexus.log.1`) și `crash.log`; pe Linux în `~/.config/nexus` | Bug-urile de pe PC-uri la care nu stăm se pot diagnostica din log |
| Config | `%APPDATA%\Nexus\nexus.ini` (`GetPrivateProfileString`) | Nativ, ușor de editat de mână; Program Files nu e scriibil |
| Interfață | Fereastră Win32 desenată de mână, cu aspectul Beamer (paleta Vernier, Hanken Grotesk), și iconiță în tray | Qt-ul din Beamer nu rulează pe XP; un singur cod pentru XP, 7 și 10/11 |
| Installer | NSIS 3, un singur `Nexus-Setup-x.y.z.exe` | NSIS suportă XP, Inno Setup 6 nu |
| Repo | github.com/limburatorul/nexus, public, GPL-3.0 | Preluăm design-ul Beamer (GPL-3) |
| Autoupdate | Principalul verifică GitHub Releases și instalează silențios; **controlatul primește update-ul de la principal**, prin legătura criptată | XP nu are TLS 1.2, deci nu poate vorbi direct cu GitHub |

## Cum funcționează
**Principal (server):** cârlige `WH_MOUSE_LL` + `WH_KEYBOARD_LL` (există de la Win2000).
- Cârligele rulează pe un fir propriu, ca un UI ocupat să nu blocheze inputul sistemului.
- Cursorul împinge într-o margine care duce la un PC conectat (și niciun buton nu e apăsat) → modul „remote”: evenimentele locale
  sunt înghițite, cursorul e parcat în centrul ecranului sub o fereastră invizibilă fără cursor (deci dispare),
  iar deplasările (dx, dy) și tastele (virtual key + scancode + flaguri) pleacă pe rețea.
- **Rezistență, colțuri, grație (0.5, port din Beamer `core/return_edge.py`):** marginea nu cedează la atingere.
  Împingerea se măsoară din **Raw Input** (`RegisterRawInputDevices` cu `RIDEV_INPUTSINK`, există de la XP), fiindcă
  hook-ul vede cursorul deja oprit de Windows la margine. Presiunea crește cu deplasarea spre exterior cât cursorul
  stă lipit de margine, scade cu mișcarea înapoi și se stinge cu 300 px/s; la `resistance` px (Crossing: Touch 0,
  Light 60, Normal 120 implicit, Firm 240; cheia `resistance` în `nexus.ini`) marginea cedează. Ultimii 8 px de la
  fiecare capăt al unui perete nu trec niciodată (butonul Close al unei ferestre maximizate, Start). După sosirea
  inputului, marginea ține 0,3 s sau până la 4 px de mișcare înapoi, ca să nu ricoșeze. Mișcările injectate de Nexus
  poartă `dwExtraInfo = "NXUS"` și Raw Input le ignoră. Cu un pointer absolut (tabletă, Remote Desktop, mouse-ul
  integrat al unei mașini virtuale) nu există împingere de măsurat, deci acolo marginea trece la atingere, ca înainte.
  Același model rulează pe controlat, cu rezistența trimisă de principal în ENTER.
- **Monitoare decalate (0.5):** o margine contează oriunde pe un monitor nu există alt monitor dincolo de ea
  (nu dreptunghiul întregului desktop). Cu un 4K și un 1080p în dreapta lui, aliniate sus, partea din dreapta a
  4K-ului de sub 1080p trece și ea. Poziția de-a lungul marginii se socotește pe toate porțiunile expuse ale acelei
  laturi, iar sosirea cade pe monitorul al cărui perete e acolo, la cel puțin 10 px de colțuri.
- **DPI mixt (0.5):** firul cârligelor și redarea pe controlat rulează *per-monitor DPI aware*
  (`SetThreadDpiAwarenessContext`, încărcat dinamic, Windows 10 1607+), ca monitoarele, cursorul și `SendInput`
  să fie toate în pixeli reali. Fereastra rămâne *system aware*. Pe XP–8 nu e nimic de făcut.
- **Buton „lipit” (0.5):** la margine, dacă hook-ul crede că un buton e ținut, Windows e întrebat din nou
  (`GetAsyncKeyState`), pentru că o eliberare peste secure desktop sau peste o fereastră elevată nu ajunge la hook.
- **Pause crossing, full-screen hold (0.5):** din tray sau de pe Crossing, marginile se pot opri pentru sesiunea
  curentă. Opțional (oprit implicit, `fullscreen_hold`), marginile țin cât o aplicație full-screen (joc) are focusul:
  `SHQueryUserNotificationState` pe Vista+ (încărcat dinamic), iar pe XP fereastra din față acoperă exact monitorul.
  Tastele de comutare merg în continuare.
- Firul cârligelor rulează cu `THREAD_PRIORITY_HIGHEST` (Windows scoate fără zgomot un hook care întârzie peste
  `LowLevelHooksTimeout`), procesul cu `ABOVE_NORMAL_PRIORITY_CLASS`, iar pe Windows 11 iese din power throttling
  (`SetProcessInformation(ProcessPowerThrottling)`, încărcat dinamic, nu există pe XP–7).
- Tastele ținute apăsate în momentul trecerii se eliberează local, nu pe celălalt PC.
- Revenirea: o margine de pe PC-ul controlat care duce înapoi (acesta raportează când cursorul o depășește),
  tasta de comutare sau, ca scăpare de urgență, **Ctrl+Alt+Pause**.
- **Tasta de comutare (0.5):** implicit dublu-tap pe **Right Ctrl**; pe pagina Keyboard se alege altă tastă
  (se apasă, Esc renunță; Enter și Space nu sunt permise) și stilul: dublu-tap sau **hold** (inputul stă pe PC-ul
  folosit ultima dată cât tasta e ținută). Cheile `switch_key` (virtual key) și `switch_style` = `tap` | `hold`.
- **Taste de salt (0.5):** fiecare PC controlat poate avea un chord (Ctrl, Alt sau Win + o tastă, cheile
  `jump`…`jump4` = modificatori << 8 | virtual key) care duce inputul direct acolo, de oriunde ar fi. Un PC
  adormit cu MAC cunoscut primește Wake-on-LAN.
- **Mai multe PC-uri (0.3):** principalul se conectează la până la 4 PC-uri controlate, fiecare cu o celulă pe o
  hartă 5 × 3 în jurul lui (pagina Crossing, cheile `peer`…`peer4` și `place`…`place4` = `x,y` în `nexus.ini`).
  Inputul trece la PC-ul de dincolo de marginea atinsă; de pe un PC controlat merge mai departe la vecinul lui
  (lanț), principalul fiind cel care rutează. Dublu-tap pe Right Ctrl duce la PC-ul folosit ultima dată.
  Sunetul vine de la PC-ul folosit ultima dată.
- Dacă legătura cade cât timp suntem în modul remote, controlul revine imediat local. **Din 0.5:** cât inputul e
  dincolo, principalul trimite PING la 250 ms, iar un PC controlat 0.5 care are inputul trimite și el PING la
  250 ms (așa, un transfer mare în celălalt sens nu-l face să pară mort). 2 s fără niciun cadru = legătură moartă:
  inputul revine acasă imediat, cu o notificare, iar legătura se reface. Restul timpului rămâne PING la 1 s și 6 s.
- **PC controlat blocat (0.5, port din Beamer `win_app/desktop_win.py`):** cât are inputul, controlatul verifică
  la 250 ms desktopul de input (`OpenInputDesktop` + numele lui, „Default” când nu e blocat; pe XP ecranul de logon
  e „Winlogon”). La două verificări la rând blocate (lock screen, UAC, Ctrl+Alt+Del) trimite `LOCKED 22`
  (unui principal mai vechi: `LEAVE`), iar principalul aduce inputul acasă și arată „unlock it at that PC”.
- **Cursorul PC-ului nefolosit (0.5):** pe un PC controlat conectat care nu are inputul, cursorul e ascuns în tot
  sistemul (`SetSystemCursor` cu un cursor gol pentru fiecare `OCR_*`) și revine cu schema utilizatorului
  (`SPI_SETCURSORS`) la ENTER, la deconectare, la ieșire, când se mișcă mouse-ul acelui PC, la un crash (din
  handlerul de excepții) și, dacă tot a rămas ascuns, la următoarea pornire (cheia `pointer_hidden`). Opțiune pe
  pagina Crossing a controlatului (`hide_pointer`, pornit implicit).
- **Wake-on-LAN (0.5):** după fiecare conexiune principalul află MAC-ul controlatului cu `SendARP` (Linux:
  `/proc/net/arp`), niciodată din textul lui `arp -a`, și îl ține în `mac`…`mac4`. Butonul „Wake” (Overview,
  tray) și tastele de salt trimit pachetul magic (6 × FF + MAC × 16, UDP 9) la broadcast.

**Controlat (client):** `SendInput` absolut pentru mouse (poziția e urmărită local, accelerația se aplică o
singură dată, pe principal) și pe virtual key pentru taste. Când cursorul e la marginea de întoarcere și
mișcarea împinge mai departe printr-o margine din masca primită în ENTER, trimite LEAVE cu poziția de-a lungul
marginii și marginea. La LEAVE sau la deconectare,
eliberează toate tastele și butoanele ținute. Pe un ecran scalat la 150%, Windows poziționează inputul absolut
pe o grilă de 1,5 px, deci precizia e ±1 px.

**Transport (implementat în `src/link.c`):** TCP pe portul 24800. Controlatul ascultă, principalul se conectează.
- Handshake în clar, 30 de octeți în fiecare sens: `"NXUS"`, versiunea legăturii (1), rolul (0 principal,
  1 controlat) și un nonce aleator de 24 de octeți.
- Cheia: `SHA-512(cheie_parolă ‖ nonce_principal ‖ nonce_controlat)`; primii 32 de octeți sunt pentru
  principal → controlat, următorii 32 pentru invers. `cheie_parolă` = SHA-512 iterat de 20.000 de ori peste
  `"nexus1" ‖ parolă`; în `nexus.ini` se salvează doar această cheie, niciodată parola.
- Cadre: `[lungime u32 LE][XSalsa20-Poly1305]`; nonce-ul e contorul de cadre al direcției și nu se trimite,
  deci un cadru reluat sau reordonat nu se deschide.
- Primul cadru în fiecare sens e `HELLO` cu numele PC-ului. Dacă nu se deschide, parolele diferă.
- **Forward secrecy (0.5):** dacă ambele HELLO-uri spun Nexus ≥ 0.5, fiecare parte trimite apoi `KEX 21` cu o
  cheie publică X25519 nouă, iar cheile devin `SHA-512(hash-ul de mai sus ‖ secret X25519 ‖ cheia publică a
  principalului ‖ cheia publică a controlatului)`, cu contoarele de la 0. Cine află parola mai târziu nu mai poate
  deschide o sesiune înregistrată. HELLO-urile sunt autentificate cu cheia parolei, deci un atacator fără parolă
  nu poate forța varianta fără KEX. Cu un capăt 0.4 nu se trimite KEX și legătura merge ca înainte (deci și
  update-ul automat spre un controlat 0.4). Overview arată „forward secret” sau „password key only”.
- Mesaje: `HELLO 1`, `PING 2`/`PONG 3` (u32 ms, la fiecare secundă; 6 s de liniște = legătură căzută; vezi mai sus
  pentru 250 ms / 2 s cât inputul e dincolo),
  `ENTER 4` [direcție, păstrează-poziția, poziție u16, ieșiri, rezistență/4 (0.5)], `LEAVE 5` [are-poziție, poziție u16, margine],
  (ieșiri = bit pe margine care duce la alt PC; un principal 0.2 trimite ENTER fără el, iar controlatul îi
  răspunde atunci cu LEAVE fără margine, deci versiunile vechi se înțeleg cu cele noi),
  `MOVE 6` [dx i16, dy i16], `BUTTON 7` [buton 1–5, apăsat], `WHEEL 8` [delta i16, orizontal], `KEY 9` [vk, scan, flaguri],
  `CLIPBOARD 10` [text UTF-8], `AUDIO 11`, `AUDIO_STATE 12`, `FILE_BEGIN 13`–`FILE_END 16`, `UPDATE_BEGIN 17`–`UPDATE_END 19`,
  `TEST 20`, iar din 0.5: `KEX 21`, `LOCKED 22`, `CLIP_BEGIN 23` [format u8][mărime u32], `CLIP_DATA 24`, `CLIP_END 25`.
  Mesajele 21–25 pleacă doar spre un PC care a spus în HELLO că are ≥ 0.5; ENTER cu al șaptelea octet la fel.

**Sunet (controlat → principal):**
- Vista și mai nou: WASAPI loopback, adică exact ce se aude pe ieșirea implicită.
- XP: `waveIn` de pe dispozitivul „Stereo Mix” / „What U Hear”. **Depinde de placa de sunet:** dacă
  driverul nu oferă acest dispozitiv, pe XP nu putem capta sunetul fără un driver virtual.
- Trimitem PCM 16-bit stereo la rata mixerului (de obicei 48 kHz, ~1.5 Mbit/s), fără codec, în bucăți de
  20 ms, pe aceeași legătură TCP criptată (`AUDIO 11` [rată u32][cadre], `AUDIO_STATE 12` [1 loopback,
  2 Stereo Mix, 3 nimic]). Bucățile de liniște completă nu se trimit. Am ales TCP în loc de UDP: nu mai e nevoie
  de un port în plus, iar cadrele de 20 ms trec printre bucățile unui transfer de fișiere.
- Principalul redă prin `waveOut`: pornește după 60 ms în coadă și aruncă bucățile care ar duce coada peste
  250 ms, ca latența să rămână mică și deriva dintre ceasurile plăcilor de sunet să nu se adune.
- Pe Vista+, controlatul se aude în continuare și pe boxele lui.

**Clipboard:** la fiecare comutare, PC-ul care cedează inputul trimite clipboardul, înaintea lui `ENTER`/`LEAVE`, dar numai dacă s-a schimbat de la ultima trimitere sau primire (`GetClipboardSequenceNumber`), ca să nu se întoarcă în ecou. UTF-8 invalid e ignorat.
- **Între PC-uri 0.5 (port din Beamer `core/protocol.py`):** textul (UTF-8, până la 256 KB) și imaginea (`CF_DIB`
  brut între Windows-uri, până la 8 MB) pleacă împreună, în bucăți: `CLIP_BEGIN` [format: 1 text, 2 DIB][mărime],
  `CLIP_DATA`…, apoi `CLIP_END` după ultimul format. Receptorul pune toate părțile complete în clipboard deodată, iar
  aplicația care face paste alege. O parte peste limită rămâne pe loc, nu se trunchiază. Un DIB primit intră în
  clipboard doar dacă antetul, culorile și pixelii sunt toți acolo (`clip_dib_ok`). Linux nu are DIB: Qt face PNG din
  DIB la primire și DIB din PNG la trimitere, deci pe fir e mereu `CF_DIB`.
- **Spre un PC mai vechi:** doar text, în `CLIPBOARD 10`, până la 64 KB, ca înainte.

**Protocol:** e același pe toate versiunile de Windows și are un octet de versiune în handshake, deci principalul și controlatul trebuie doar să aibă aceeași versiune de protocol, nu aceeași versiune de Windows.

**Fișiere (`src/files.c`):** copiezi fișiere sau foldere în Explorer (clipboard `CF_HDROP`). La comutare,
un fir separat le trimite în bucăți de 32 KB, intercalate cu inputul și sunetul: `FILE_BEGIN 13` [număr u32][total u64],
apoi pentru fiecare element `FILE 14` [e-folder u8][mărime u64][cale relativă UTF-8] și `FILE_DATA 15`,
iar la final `FILE_END 16`. Părinții vin înaintea conținutului. Receptorul scrie direct de pe firul de rețea în
`Desktop\Nexus Received\<dată oră>\` și pune elementele de nivel superior în clipboard ca `CF_HDROP`, deci
**Ctrl+V** le lipește unde vrei.
- Căile primite sunt validate: doar nume simple, nimic absolut, fără `..` și fără caractere interzise; orice
  abatere anulează lotul, iar un folder rămas gol se șterge.
- Nu există limită de mărime; receptorul refuză doar un lot care nu încape pe discul lui. Joncțiunile nu sunt urmate. Un fișier
  blocat e sărit. Cât timp un transfer e în curs, o nouă copiere rămâne pe loc.

## Etape (fiecare se testează înainte de următoarea)
1. **M0** ✅ Toolchain + schelet: `nexus.exe` cu fereastră și tray, instanță unică, installer NSIS, build XP-compatibil verificat (`tools/check_xp_imports.py`).
2. **M1** ✅ (testat pe Win11 contra unor parteneri falși; **netestat încă pe XP real**) Rețea + criptare + mouse/tastatură: margine, dublu-tap, urgență, reconectare. Teste: `build/link_test.exe` (în build), `tests/fake_main.c`, `tests/fake_controlled.c`.
3. **M2** ✅ Clipboard text (testat pe Win11 în ambele sensuri cu `tests/fake_main.c`).
4. **M3** ✅ Sunet (WASAPI pe Vista+, Stereo Mix pe XP). Testat pe Win11: captura cu `fake_main audio`, redarea cu `fake_controlled 3 tone`. **Stereo Mix pe XP netestat.**
5. **M4** ✅ Transfer fișiere prin copy/paste (testat pe Win11 în ambele sensuri, inclusiv refuzul unei căi `..\`, cu `fake_main files`).
6. **M5** ✅ Autoupdate (`src/update.c`): principalul verifică `releases/latest` la un minut după pornire și apoi la 6 ore,
   descarcă `Nexus-Setup-X.Y.Z.exe` în `%APPDATA%\Nexus\updates` și arată în sidebar „Install Nexus X.Y.Z”. Un click
   trimite întâi installerul controlatului (dacă e în urmă), apoi îl rulează local cu `/S`; installerul închide Nexus și
   îl repornește. Un controlat care se conectează mai târziu cu o versiune mai veche primește automat installerul
   versiunii principalului (`UPDATE_BEGIN 17` [mărime u32][versiune], `UPDATE_DATA 18`, `UPDATE_END 19`). Controlatul
   rulează doar o versiune mai nouă decât a lui și doar un fișier complet care începe cu `MZ`. HELLO poartă acum și
   versiunea. Release-urile le face `.github/workflows/release.yml` la un tag `vX.Y.Z`. Pe Vista+, installerul cere
   confirmarea UAC pe ecranul acelui PC; pe XP (cont de administrator) e silențios.
   Testat pe Win11: primirea și rularea (`fake_main update`) și descărcarea reală a release-ului v0.2.0 de pe GitHub.
7. **Descoperire în rețea** ✅ UDP 24802: `"NXUS?"` întreabă, fiecare Nexus răspunde `"NXUS!"` + rol + versiune + NUL + nume.
   Principalul întreabă la 3 s (și pe fiecare /24 local, fiindcă Windows trimite 255.255.255.255 pe un singur adaptor);
   pe pagina Connection apar PC-urile controlate găsite, un click completează adresa. Nimic din răspuns nu e secret.
8. **Linux** (Python + PySide6, `linux/nexus`), refolosind back-end-urile Wayland din Beamer (`linux/beamer`):
   - controlat: injectare și clipboard prin portalul RemoteDesktop (cere permisiunea o dată, apoi token);
   - principal: captură prin portalul InputCapture (KDE 6.6 are doar v1, fără token: **cere permisiunea la fiecare pornire**),
     cu bariere pe fiecare margine care duce la un PC conectat de pe hartă; aceeași rutare ca pe Windows;
   - sunet prin `parec`/`pacat` (PipeWire sau PulseAudio), fișierele copiate ca `text/uri-list`;
   - aceeași fereastră, aceleași pagini și același mod de test ca pe Windows. **X11 netestat.**
9. **Teste automate** (`tests/driver.py`): `nexus /test` (Windows) și `python -m nexus --test` (Linux) rulează un
   profil separat (`test.ini`, port 24810, fără cârlige) și primesc comenzi pe 24811, autentificate cu cheia legăturii.
   Driverul verifică text ↔, cursor și fișiere ↔ între oricare două PC-uri. Trec: Win11 → XP, Win11 → Kubuntu, Kubuntu → XP.
   `tests/multi.py` verifică lanțul pe hartă (principal → B → C → B → principal, cu clipboardul pe drum): trece cu
   Kubuntu principal, XP la dreapta lui și Manjaro la dreapta lui XP.
10. **0.5** (porturi din Beamer, vezi `Nexus-Beamer-gap.md`): rezistență la margine + colțuri + grație, monitoare
   decalate și DPI mixt, butonul „lipit”, legătură moartă în 2 s, prioritate pe hook și fără power throttling,
   controlat blocat, imagini și text mare în clipboard, tray cu starea/meniu/notificări, Pause crossing și
   full-screen hold, log + crash log, scroll în setări, cursorul ascuns pe PC-ul nefolosit, tastă de comutare
   configurabilă (tap/hold) și taste de salt, Wake-on-LAN, forward secrecy. Teste: `tests/edge_test.c` + `.py`
   (pereți, colțuri, rezistență, stingere, grație), `tests/link_test.c` + `.py` (X25519, compatibilitate 0.4,
   bucățile clipboardului, validarea DIB), `tests/driver.py run` (text 200 KB și imagine 640×480 în ambele sensuri,
   forward secrecy) și `tests/driver.py beat` (VM înghețat: inputul acasă în ~2 s).
11. **Următoarele (plan, nu sunt în 0.5):**
   - **Pairing cu cod de 6 cifre** în locul parolei comune: controlatul afișează un cod valabil un minut, o singură
     dată; principalul îl introduce; un PAKE (CPace peste X25519 din TweetNaCl, cu elligator2 + HKDF scrise peste
     SHA-512, după `PAIRING.md` și vectorii din Beamer) dă fiecărui principal un token propriu, salvat în ini lângă
     `peer`. Un atacator are o singură încercare per cod. Varianta intermediară: principalul generează un secret lung
     și îl arată ca cod, iar parola rămâne pentru PC-urile vechi. Versiunea legăturii crește, cu fallback la parolă.
   - **Control în ambele sensuri**: ambele roluri în același proces (cârligele pe principal, redarea pe controlat
     există deja), câte o legătură pe sens între două PC-uri, harta pe fiecare parte și un switch per direcție
     („This PC drives others” / „Others drive this PC”), ca în Beamer. Cere rutare când două PC-uri vor inputul în
     același timp: câștigă cel care are mâna pe propriul mouse.

## Nu facem (cel puțin în v1)
- Ctrl+Alt+Del către PC-ul controlat: cere un serviciu Windows sau winlogon. Pentru asta folosim combinația de urgență.
- Ecranul de logon / UAC securizat pe controlat: același motiv.
- Efectele grafice din Beamer.

## Limitări cunoscute
- Pe un PC controlat cu Vista sau mai nou, inputul nu ajunge în ferestrele pornite ca administrator (UIPI)
  cât timp Nexus rulează neelevat. Soluția: pornire elevată prin Task Scheduler la logare.
- Linux principal pe Wayland: fără scurtătură pentru a trece (nu există hook global de taste); se trece prin margine
  („Send input across” din tray împinge cursorul prin margine cu mișcare injectată; KWin, cel puțin Plasma 6.6 cu
  InputCapture v1, nu declanșează bariera pentru mișcare injectată, așa că acolo apare doar o notificare), iar
  înapoi cu tasta de comutare sau Ctrl+Alt+Pause, cât timp inputul e capturat. Stilul hold aduce inputul acasă la
  eliberarea tastei. Tastele de salt merg doar cât inputul e dincolo. Fără captură, inputul nu poate sta dincolo
  (desktopul nu dă nicio tastă), deci comanda de test `cross` pe un principal Linux e anulată imediat de
  `capture_portal` cât timp captura e acordată: e comportamentul voit din Beamer, nu un bug; testele cu un
  principal Linux rulează cu dialogul de captură nerăspuns (ca la 0.4) sau prin margine cu mouse-ul real.
- Linux: cursorul PC-ului nefolosit nu se poate ascunde pe Wayland, iar full-screen hold nu are ce citi; lipsesc.
- Monitoare decalate cu un gol între ele pe aceeași latură: poziția de-a lungul marginii se socotește peste gol.
- Pe un PC controlat 0.4 sau mai vechi: fără rezistență pe marginea lui (trece la atingere), fără imagini în
  clipboard și fără forward secrecy; update-ul automat îl aduce la 0.5.
- Un PC controlat blocat se vede doar cât are inputul; ENTER spre el aduce inputul înapoi după ~0,5 s.
