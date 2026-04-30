#define _GNU_SOURCE
/*
 * Demon synchronizujący dwa katalogi.
 *
 * Program przyjmuje ścieżkę katalogu źródłowego i docelowego.
 * Po poprawnej walidacji uruchamia się jako demon, czyli proces działający w tle.
 *
 * Demon cyklicznie:
 * - zasypia na określoną liczbę sekund,
 * - po obudzeniu porównuje katalog źródłowy z docelowym,
 * - kopiuje nowe lub zmienione pliki,
 * - usuwa z katalogu docelowego pliki, których nie ma już w źródle.
 *
 * Opcje:
 * -R             synchronizacja rekurencyjna podkatalogów,
 * -s sekundy     czas uśpienia demona,
 * -m bajty       próg rozmiaru pliku, od którego używane jest mmap.
 *
 * Sygnały:
 * SIGUSR1        natychmiastowe wybudzenie demona,
 * SIGTERM/SIGINT zakończenie działania demona.
 *
 * Małe pliki kopiowane są przez read/write.
 * Duże pliki kopiowane są przez mmap/write.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SLEEP_SECONDS 300
#define DEFAULT_MMAP_THRESHOLD (1024 * 1024)

/*
 * Flagi ustawiane przez sygnały.
 * Muszą być sig_atomic_t, bo są modyfikowane asynchronicznie z handlera.
 */
static volatile sig_atomic_t g_wake_requested = 0;
static volatile sig_atomic_t g_stop_requested = 0;

/*
 * Handler sygnałów:
 * - SIGUSR1: tylko budzi demona przed końcem sleep,
 * - SIGTERM/SIGINT: ustawia zakończenie i również wybudza pętlę.
 */
static void signal_handler(int signo) {
    if (signo == SIGUSR1) {
        g_wake_requested = 1;
    } else if (signo == SIGTERM || signo == SIGINT) {
        g_stop_requested = 1;
        g_wake_requested = 1;
    }
}

/*
 * Prosty wrapper na syslog: każdy komunikat dostaje własny znacznik daty/godziny.
 */
static void log_with_date(int priority, const char *fmt, ...) {
    char timebuf[64];
    time_t now = time(NULL);
    struct tm tm_now;

    if (localtime_r(&now, &tm_now) != NULL) {
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_now);
    } else {
        strncpy(timebuf, "unknown-time", sizeof(timebuf));
        timebuf[sizeof(timebuf) - 1] = '\0';
    }

    // Skladamy wiadomosc koncowa z varargs w buforze tymczasowym.
    char msgbuf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgbuf, sizeof(msgbuf), fmt, args);
    va_end(args);

    syslog(priority, "[%s] %s", timebuf, msgbuf);
}

/*
 * Łączy katalog + nazwę wpisu do jednej ścieżki i pilnuje przepełnienia bufora.
 */
static int join_path(char *out, size_t out_sz, const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    int needs_slash = (dlen > 0 && dir[dlen - 1] != '/');
    int written = snprintf(out, out_sz, "%s%s%s", dir, needs_slash ? "/" : "", name);
    if (written < 0 || (size_t)written >= out_sz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/*
 * Ustawia w pliku docelowym te same czasy dostępu/modyfikacji co w źródle.
 * Dzięki temu przy następnym przebiegu nie kopiujemy bez potrzeby.
 */
static int set_dest_timestamps(const char *dst_path, const struct stat *src_st) {
    struct timespec ts[2];
    ts[0] = src_st->st_atim;
    ts[1] = src_st->st_mtim;
    return utimensat(AT_FDCWD, dst_path, ts, 0);
}

/*
 * Kopiowanie „małego” pliku klasycznie przez read/write w pętli.
 */
static int copy_small_rw(int src_fd, int dst_fd) {
    char buf[8192];
    while (1) {
        ssize_t rd = read(src_fd, buf, sizeof(buf));
        if (rd == 0) {
            return 0;
        }
        if (rd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        ssize_t written_total = 0;
        while (written_total < rd) {
            // Zapisujemy tyle, ile zostalo z aktualnie odczytanego bloku.
            ssize_t wr = write(dst_fd, buf + written_total, (size_t)(rd - written_total));
            if (wr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            written_total += wr;
        }
    }
}

/*
 * Kopiowanie „dużego” pliku:
 * mapujemy całe źródło (mmap), a następnie wypychamy bajty write() do celu.
 */
static int copy_large_mmap(int src_fd, int dst_fd, off_t size) {
    if (size == 0) {
        return 0;
    }

    // Mapujemy cale zrodlo tylko do odczytu; kopiowanie bedzie write() do celu.
    void *map = mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, src_fd, 0);
    if (map == MAP_FAILED) {
        return -1;
    }

    const char *cursor = (const char *)map;
    off_t left = size;
    while (left > 0) {
        ssize_t wr = write(dst_fd, cursor, (size_t)left);
        if (wr < 0) {
            if (errno == EINTR) {
                continue;
            }
            munmap(map, (size_t)size);
            return -1;
        }
        cursor += wr;
        left -= wr;
    }

    if (munmap(map, (size_t)size) < 0) {
        return -1;
    }

    return 0;
}

/*
 * Kopiuje pojedynczy plik źródło->cel i dobiera metodę kopii wg progu -m.
 * Dodatkowo zachowuje tryb (chmod) i czasy (utimensat).
 */
static int copy_file(const char *src_path, const char *dst_path, size_t mmap_threshold, const struct stat *src_st) {
    int src_fd = -1;
    int dst_fd = -1;
    int rc = -1;

    // Otwieramy zrodlo do odczytu binarnego.
    src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        log_with_date(LOG_ERR, "Nie mozna otworzyc zrodla '%s': %s", src_path, strerror(errno));
        goto cleanup;
    }

    // Otwieramy/zakladamy plik docelowy i obcinamy poprzednia zawartosc.
    dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, src_st->st_mode & 0777);
    if (dst_fd < 0) {
        log_with_date(LOG_ERR, "Nie mozna otworzyc celu '%s': %s", dst_path, strerror(errno));
        goto cleanup;
    }

    // Wybor strategii kopiowania na podstawie rozmiaru i progu -m.
    if ((off_t)mmap_threshold > 0 && src_st->st_size >= (off_t)mmap_threshold) {
        if (copy_large_mmap(src_fd, dst_fd, src_st->st_size) < 0) {
            log_with_date(LOG_ERR, "Blad kopiowania mmap '%s' -> '%s': %s", src_path, dst_path, strerror(errno));
            goto cleanup;
        }
    } else {
        if (copy_small_rw(src_fd, dst_fd) < 0) {
            log_with_date(LOG_ERR, "Blad kopiowania read/write '%s' -> '%s': %s", src_path, dst_path, strerror(errno));
            goto cleanup;
        }
    }

    if (fchmod(dst_fd, src_st->st_mode & 0777) < 0) {
        log_with_date(LOG_WARNING, "Nie mozna ustawic chmod dla '%s': %s", dst_path, strerror(errno));
    }

    if (close(dst_fd) < 0) {
        dst_fd = -1;
        goto cleanup;
    }
    dst_fd = -1;

    if (set_dest_timestamps(dst_path, src_st) < 0) {
        log_with_date(LOG_WARNING, "Nie mozna ustawic czasow '%s': %s", dst_path, strerror(errno));
    }

    log_with_date(LOG_INFO, "Skopiowano plik: '%s' -> '%s'", src_path, dst_path);
    rc = 0;

cleanup:
    if (src_fd >= 0) {
        close(src_fd);
    }
    if (dst_fd >= 0) {
        close(dst_fd);
    }
    return rc;
}

static int remove_tree(const char *path);

/*
 * Dla trybu -R: zapewnia, że w katalogu docelowym istnieje dany podkatalog.
 * Jeśli pod tą nazwą istnieje inny typ obiektu, usuwa go i tworzy katalog.
 */
static int ensure_dir_exists(const char *path, mode_t mode) {
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        }
        if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) || S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) ||
            S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) {
            if (unlink(path) < 0) {
                return -1;
            }
        } else {
            errno = ENOTDIR;
            return -1;
        }
    } else if (errno != ENOENT) {
        return -1;
    }

    if (mkdir(path, mode & 0777) < 0) {
        if (errno == EEXIST) {
            return 0;
        }
        return -1;
    }
    return 0;
}

/*
 * Decyzja „czy kopiować plik”:
 * - brak pliku w celu -> kopiujemy,
 * - inny typ obiektu w celu -> usuwamy i kopiujemy,
 * - starszy mtime lub inny rozmiar -> kopiujemy,
 * - w przeciwnym razie pomijamy.
 */
static int maybe_copy_regular(const char *src_path, const char *dst_path, size_t mmap_threshold) {
    struct stat src_st;
    struct stat dst_st;

    if (lstat(src_path, &src_st) < 0) {
        return -1;
    }
    if (!S_ISREG(src_st.st_mode)) {
        return 0;
    }

    // Decyzja biznesowa: czy ten plik trzeba skopiowac w tej iteracji.
    bool need_copy = false;
    if (lstat(dst_path, &dst_st) < 0) {
        if (errno == ENOENT) {
            need_copy = true;
        } else {
            return -1;
        }
    } else {
        if (!S_ISREG(dst_st.st_mode)) {
            if (S_ISDIR(dst_st.st_mode)) {
                if (remove_tree(dst_path) < 0) {
                    return -1;
                }
            } else {
                if (unlink(dst_path) < 0) {
                    return -1;
                }
            }
            need_copy = true;
        } else if (src_st.st_mtime > dst_st.st_mtime || src_st.st_size != dst_st.st_size) {
            need_copy = true;
        }
    }

    if (!need_copy) {
        return 0;
    }

    return copy_file(src_path, dst_path, mmap_threshold, &src_st);
}

/*
 * Rekurencyjne usuwanie dowolnego poddrzewa w katalogu docelowym.
 * Używane, gdy w trybie -R znajdziemy katalog/docelowy element bez odpowiednika w źródle.
 */
static int remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }

    if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) || S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) ||
        S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) {
        if (unlink(path) < 0) {
            return -1;
        }
        log_with_date(LOG_INFO, "Usunieto plik: '%s'", path);
        return 0;
    }

    if (!S_ISDIR(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }

    // Otwieramy katalog, by przejsc po wszystkich dzieciach i usunac je rekurencyjnie.
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }

    struct dirent *ent;
    int rc = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char child[PATH_MAX];
        if (join_path(child, sizeof(child), path, ent->d_name) < 0) {
            rc = -1;
            break;
        }
        if (remove_tree(child) < 0) {
            rc = -1;
            break;
        }
    }

    int saved_errno = errno;
    closedir(dir);
    errno = saved_errno;

    if (rc < 0) {
        return -1;
    }

    if (rmdir(path) < 0) {
        return -1;
    }

    log_with_date(LOG_INFO, "Usunieto katalog: '%s'", path);
    return 0;
}

/*
 * Główna funkcja synchronizacji katalogów.
 *
 * Etap 1 (przejście po źródle):
 * - pliki regularne: decyzja/copy,
 * - katalogi (tylko -R): tworzenie brakujących i synchronizacja podkatalogu.
 *
 * Etap 2 (przejście po celu):
 * - usuwanie obiektów, których nie ma już w źródle,
 * - przy -R także usuwanie całych nadmiarowych podkatalogów.
 */
static int sync_dirs(const char *src_dir, const char *dst_dir, bool recursive, size_t mmap_threshold) {
    DIR *src = opendir(src_dir);
    if (!src) {
        log_with_date(LOG_ERR, "Nie mozna otworzyc katalogu zrodla '%s': %s", src_dir, strerror(errno));
        return -1;
    }

    struct dirent *ent;
    int rc = 0;

    // Dla kazdego wpisu zrodla: plik -> maybe_copy, katalog -> rekurencja (tylko -R).
    while ((ent = readdir(src)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char src_path[PATH_MAX];
        char dst_path[PATH_MAX];
        if (join_path(src_path, sizeof(src_path), src_dir, ent->d_name) < 0 ||
            join_path(dst_path, sizeof(dst_path), dst_dir, ent->d_name) < 0) {
            log_with_date(LOG_ERR, "Zbyt dluga sciezka podczas synchronizacji");
            rc = -1;
            break;
        }

        struct stat src_st;
        if (lstat(src_path, &src_st) < 0) {
            log_with_date(LOG_WARNING, "Nie mozna pobrac stat dla '%s': %s", src_path, strerror(errno));
            continue;
        }

        if (S_ISREG(src_st.st_mode)) {
            if (maybe_copy_regular(src_path, dst_path, mmap_threshold) < 0) {
                log_with_date(LOG_WARNING, "Blad synchronizacji pliku '%s': %s", src_path, strerror(errno));
            }
        } else if (recursive && S_ISDIR(src_st.st_mode)) {
            if (ensure_dir_exists(dst_path, src_st.st_mode) < 0) {
                log_with_date(LOG_WARNING, "Nie mozna przygotowac katalogu '%s': %s", dst_path, strerror(errno));
                continue;
            }
              // Rekurencyjnie synchronizujemy znaleziony podkatalog.
        if (sync_dirs(src_path, dst_path, recursive, mmap_threshold) < 0) {
                log_with_date(LOG_WARNING, "Blad synchronizacji podkatalogu '%s'", src_path);
            }
        }
    }

    int saved_errno = errno;
    closedir(src);
    errno = saved_errno;

    if (rc < 0) {
        return -1;
    }

    // Etap 2: przechodzimy po celu i usuwamy to, czego nie ma w zrodle.
    DIR *dst = opendir(dst_dir);
    if (!dst) {
        log_with_date(LOG_ERR, "Nie mozna otworzyc katalogu celu '%s': %s", dst_dir, strerror(errno));
        return -1;
    }

    while ((ent = readdir(dst)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char src_path[PATH_MAX];
        char dst_path[PATH_MAX];
        if (join_path(src_path, sizeof(src_path), src_dir, ent->d_name) < 0 ||
            join_path(dst_path, sizeof(dst_path), dst_dir, ent->d_name) < 0) {
            log_with_date(LOG_ERR, "Zbyt dluga sciezka podczas czyszczenia");
            rc = -1;
            break;
        }

        struct stat dst_st;
        if (lstat(dst_path, &dst_st) < 0) {
            continue;
        }

        struct stat src_st;
        bool src_exists = (lstat(src_path, &src_st) == 0);

        if (S_ISREG(dst_st.st_mode)) {
            if (!src_exists || !S_ISREG(src_st.st_mode)) {
                if (unlink(dst_path) == 0) {
                    log_with_date(LOG_INFO, "Usunieto plik z celu: '%s'", dst_path);
                } else {
                    log_with_date(LOG_WARNING, "Nie mozna usunac pliku '%s': %s", dst_path, strerror(errno));
                }
            }
        } else if (recursive && S_ISDIR(dst_st.st_mode)) {
            if (!src_exists || !S_ISDIR(src_st.st_mode)) {
                if (remove_tree(dst_path) < 0) {
                    log_with_date(LOG_WARNING, "Nie mozna usunac katalogu '%s': %s", dst_path, strerror(errno));
                }
            }
        }
    }

    saved_errno = errno;
    closedir(dst);
    errno = saved_errno;

    return rc;
}

/*
 * Klasyczna daemonizacja (double-fork):
 * 1) fork + wyjście rodzica,
 * 2) setsid() -> nowa sesja i brak controlling terminal,
 * 3) drugi fork -> brak możliwości odzyskania terminala,
 * 4) chdir("/"), umask(0),
 * 5) podpięcie stdin/stdout/stderr do /dev/null.
 */
static int daemonize_process(void) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        _exit(EXIT_SUCCESS);
    }

    // Tworzymy nowa sesje i odcinamy controlling terminal.
    if (setsid() < 0) {
        return -1;
    }

    // Ignorujemy HUP, bo demon nie powinien konczyc po zamknieciu terminala.
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        _exit(EXIT_SUCCESS);
    }

    // Przechodzimy do / aby nie blokowac montowanego katalogu roboczego.
    if (chdir("/") < 0) {
        return -1;
    }

    umask(0);

    // Podpinamy stdin/stdout/stderr pod /dev/null - pelne odpiecie I/O od terminala.
    int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0) {
        return -1;
    }

    if (dup2(devnull, STDIN_FILENO) < 0 || dup2(devnull, STDOUT_FILENO) < 0 || dup2(devnull, STDERR_FILENO) < 0) {
        close(devnull);
        return -1;
    }

    if (devnull > STDERR_FILENO) {
        close(devnull);
    }

    return 0;
}

/*
 * Parsowanie argumentów.
 * -R: włącza rekurencyjną synchronizację.
 * -s: ustawia czas snu między przebiegami synchronizacji.
 * -m: ustawia próg bajtów dla wyboru mmap.
 */
static int parse_args(int argc, char **argv, bool *recursive, unsigned int *sleep_seconds,
                      size_t *mmap_threshold, const char **src, const char **dst) {
    *recursive = false;
    *sleep_seconds = DEFAULT_SLEEP_SECONDS;
    *mmap_threshold = DEFAULT_MMAP_THRESHOLD;

    int opt;
    // getopt przetwarza opcje w dowolnej kolejnosci przed argumentami pozycyjnymi.
    while ((opt = getopt(argc, argv, "Rs:m:")) != -1) {
        switch (opt) {
            case 'R':
                *recursive = true;
                break;
            case 's': {
                char *end = NULL;
                // Konwersja interwalu snu (sekundy) z tekstu na liczbe dodatnia.
                unsigned long value = strtoul(optarg, &end, 10);
                if (!end || *end != '\0' || value == 0 || value > UINT32_MAX) {
                    return -1;
                }
                *sleep_seconds = (unsigned int)value;
                break;
            }
            case 'm': {
                char *end = NULL;
                // Konwersja progu mmap (bajty). 0 oznacza: zawsze read/write.
                unsigned long long value = strtoull(optarg, &end, 10);
                if (!end || *end != '\0') {
                    return -1;
                }
                *mmap_threshold = (size_t)value;
                break;
            }
            default:
                return -1;
        }
    }

    if (argc - optind != 2) {
        return -1;
    }

    *src = argv[optind];
    *dst = argv[optind + 1];
    return 0;
}

/*
 * Punkt wejścia:
 * 1) walidacja argumentów i katalogów,
 * 2) daemonizacja,
 * 3) konfiguracja sysloga i sygnałów,
 * 4) pętla: sleep -> wybudzenie naturalne/sygnałowe -> synchronizacja.
 */
int main(int argc, char **argv) {
    bool recursive;
    unsigned int sleep_seconds;
    size_t mmap_threshold;
    const char *src_path;
    const char *dst_path;

    // Parsujemy opcje i argumenty wymagane; blad => natychmiastowe wyjscie z usage.
    if (parse_args(argc, argv, &recursive, &sleep_seconds, &mmap_threshold, &src_path, &dst_path) < 0) {
        dprintf(STDERR_FILENO,
                "Uzycie: %s [-R] [-s sekundy] [-m prog_bajtow] <katalog_zrodlowy> <katalog_docelowy>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    struct stat src_st;
    struct stat dst_st;

    // Sprawdzamy czy zrodlo istnieje i jest katalogiem.
    if (stat(src_path, &src_st) < 0 || !S_ISDIR(src_st.st_mode)) {
        dprintf(STDERR_FILENO, "Blad: '%s' nie jest katalogiem.\n", src_path);
        return EXIT_FAILURE;
    }
    // Sprawdzamy czy cel istnieje i jest katalogiem.
    if (stat(dst_path, &dst_st) < 0 || !S_ISDIR(dst_st.st_mode)) {
        dprintf(STDERR_FILENO, "Blad: '%s' nie jest katalogiem.\n", dst_path);
        return EXIT_FAILURE;
    }
    // Zamieniamy ścieżki na bezwzględne przed daemonizacją.
    // Po daemonizacji proces wykona chdir("/"), więc ścieżki względne mogłyby przestać działać.
    char src_abs[PATH_MAX];
    char dst_abs[PATH_MAX];
    
    if (realpath(src_path, src_abs) == NULL) {
        dprintf(STDERR_FILENO, "Blad realpath dla zrodla '%s': %s\n", src_path, strerror(errno));
        return EXIT_FAILURE;
    }
    
    if (realpath(dst_path, dst_abs) == NULL) {
        dprintf(STDERR_FILENO, "Blad realpath dla celu '%s': %s\n", dst_path, strerror(errno));
        return EXIT_FAILURE;
    }
    
    src_path = src_abs;
    dst_path = dst_abs;

    // Zamieniamy proces foreground na poprawnego demona.
    if (daemonize_process() < 0) {
        dprintf(STDERR_FILENO, "Blad daemonizacji: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    // Inicjalizacja logowania do sysloga (facility: daemon).
    openlog("sync_daemon", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    // Rejestrujemy jeden handler dla sygnalow wybudzania i zatrzymania.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGUSR1, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0 || sigaction(SIGINT, &sa, NULL) < 0) {
        log_with_date(LOG_ERR, "Nie mozna ustawic obslugi sygnalow: %s", strerror(errno));
        closelog();
        return EXIT_FAILURE;
    }

    log_with_date(LOG_INFO,
                  "Demon uruchomiony. SRC='%s' DST='%s' R=%d sleep=%u mmap_threshold=%zu PID=%d",
                  src_path, dst_path, recursive ? 1 : 0, sleep_seconds, mmap_threshold, getpid());

    // Glowna petla pracy demona: sleep -> wake -> sync.
    while (!g_stop_requested) {
        g_wake_requested = 0;
        log_with_date(LOG_INFO, "Demon zasypia na %u sekund.", sleep_seconds);

        // sleep moze zostac przerwany sygnalem - trzymamy pozostaly czas.
        unsigned int remaining = sleep_seconds;
        while (remaining > 0 && !g_wake_requested && !g_stop_requested) {
            remaining = sleep(remaining);
        }

        if (g_stop_requested) {
            break;
        }

        if (g_wake_requested) {
            log_with_date(LOG_INFO, "Demon obudzony sygnalem SIGUSR1.");
        } else {
            log_with_date(LOG_INFO, "Demon obudzony naturalnie po uplywie czasu.");
        }

        // Po wybudzeniu wykonujemy pojedynczy cykl synchronizacji drzew katalogow.
        if (sync_dirs(src_path, dst_path, recursive, mmap_threshold) < 0) {
            log_with_date(LOG_ERR, "Synchronizacja zakonczona bledem.");
        }
    }

    log_with_date(LOG_INFO, "Demon konczy dzialanie.");
    closelog();
    return EXIT_SUCCESS;
}
