# Projekt 1: demon synchronizujący katalogi (Linux, C)

Program `sync_daemon` synchronizuje katalog źródłowy z docelowym jako demon systemowy.

## Budowanie

```bash
make
```

## Użycie

```bash
./sync_daemon [-R] [-s sekundy] [-m próg_bajtów] <katalog_źródłowy> <katalog_docelowy>
```

### Opcje

- `-R` – synchronizacja rekurencyjna podkatalogów.
- `-s sekundy` – interwał snu demona (domyślnie: 300 sekund).
- `-m próg_bajtów` – próg rozmiaru pliku dla strategii kopiowania:
  - `< próg` → kopiowanie `read/write`,
  - `>= próg` → kopiowanie `mmap/write`.

## Zachowanie

- jeśli plik regularny istnieje w źródle i nie istnieje w celu → kopiowanie,
- jeśli plik regularny w źródle jest nowszy (lub różni się rozmiarem) → kopiowanie,
- jeśli plik regularny istnieje w celu, ale nie ma go w źródle → usuwanie,
- przy `-R` analogiczna obsługa podkatalogów, w tym usuwanie nadmiarowych katalogów z celem,
- ignorowane są wpisy inne niż zwykłe pliki (oraz katalogi bez `-R`).

## Sygnały

- `SIGUSR1` – natychmiastowe obudzenie demona,
- `SIGTERM` / `SIGINT` – zakończenie działania.

Przykład wysłania sygnału:

```bash
kill -USR1 <pid_demona>
```

## Logowanie

Program loguje do sysloga (`LOG_DAEMON`, identyfikator `sync_daemon`) wszystkie istotne akcje:
- zaśnięcie i obudzenie,
- kopiowanie,
- usuwanie,
- błędy.
