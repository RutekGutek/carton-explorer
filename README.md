# Carton Explorer (Linux)

Szybki, nowoczesny eksplorator plików — port wersji na Windows na Linuksa (C++17 + Qt6 Widgets, własne rysowanie, ciemny granatowo-fioletowy motyw, zaokrąglone okno).

## Funkcje
- Natychmiastowe wyszukiwanie z indeksu w pamięci: `*.txt`, `test_*.png`, `?`, `a | b`, `!x`/`-x`, `"fraza"`, `ext:`, `size:`, `dm:`, `folder:`/`file:`, `path:`, `re:`.
- Rozmiary folderów liczone przyrostowo w tle (indeks wielowątkowy, aktualizacja na żywo przez inotify).
- Szybkie kopiowanie/przenoszenie: rename na tym samym FS, reflink (FICLONE), `copy_file_range`, wielowątkowość dobierana do SSD/HDD/sieci; konflikty (Zamień/Pomiń/Zachowaj oba), pauza i anulowanie, pasek postępu z prędkością i ETA.
- Drag & drop (Ctrl = kopiuj, Shift = przenieś, domyślnie przenosi w obrębie tego samego dysku), Ctrl+C/X/V zgodne ze schowkiem Nautilusa/Dolphina.
- Karty, okruszki, Szybki dostęp, dyski/montowania, Kosz (freedesktop), „Otwórz za pomocą”, terminal, właściwości.

## Budowanie
Zależności: `cmake`, `ninja`, `g++`, Qt6 (`qt6-base-dev`; opcjonalnie Qt6 DBus dla postępu w launcherze).

    ./build.sh && ./install.sh     # instalacja w ~/.local
    carton [katalog]

Testy: `build/test_core`, `build/test_copy`.

## Uwagi dla Linuksa
- Limit inotify: przy dużych drzewach zwiększ `fs.inotify.max_user_watches` (np. `sudo sysctl fs.inotify.max_user_watches=1048576`). Gdy limit zostanie osiągnięty, widok odświeża się przy nawigacji.
- Rozmiary folderów nie przekraczają granic montowań (jak `du -x`); każdy zamontowany system plików jest indeksowany osobno.
- Dowiązania symboliczne nie są śledzone (jak liście).
- Wyszukiwanie jest niewrażliwe na wielkość liter; nazwy w indeksie są dokładne.
- Ustawienia: `~/.config/carton/settings.ini`.
- Pod Waylandem przeciąganie/zmiana rozmiaru okna używa kompozytora (startSystemMove/Resize).
