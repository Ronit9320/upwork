#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ccheck.h"
#include <stdio.h>
#include <sys/stat.h>

/*
 * Options (see the assignment document for details):
 *   -w           play white
 *   -b           play black
 *   -r           randomized play
 *   -v           give info about search
 *   -d           don't try to use X window system display
 *   -t           tournament mode
 *   -a <num>     set average time per move (in seconds)
 *   -i <file>    initialize from saved game score
 *   -o <file>    specify transcript file name
 */


// ======= Runtime config (from args) =======
typedef struct Config {
    bool play_white_engine;   // -w
    bool play_black_engine;   // -b
    bool randomized_play;     // -r -> sets global randomized
    bool verbose_stats;       // -v -> sets global verbose
    bool no_display;          // -d
    bool tournament_mode;     // -t
    int  avg_time;            // -a <sec> -> sets global avgtime
    const char *init_file;    // -i <file>
    const char *transcript;   // -o <file>
} Config;

// ======= Child bookkeeping =======
static pid_t g_disp_pid = -1;   // xdisp child pid (if any)
static pid_t g_eng_pid  = -1;   // engine child pid (if any)

// Pipes as FILE* for convenience
static FILE *g_disp_in  = NULL; // parent -> display (child stdin)
static FILE *g_disp_out = NULL; // parent <- display (child stdout)
static FILE *g_eng_in   = NULL; // parent -> engine (child stdin)
static FILE *g_eng_out  = NULL; // parent <- engine (child stdout)

// Transcript file
static FILE *g_tx = NULL;

// ======= Signal flags (set only in handlers) =======
static volatile sig_atomic_t g_got_sigint  = 0;
static volatile sig_atomic_t g_got_sigterm = 0;
static volatile sig_atomic_t g_got_sigpipe = 0;
static volatile sig_atomic_t g_got_sigchld = 0;

// ======= Globals from ccheck.h to set =======
// int randomized = 0;  // NOLINT: provided by library
// int verbose    = 0;  // NOLINT: provided by library
// int avgtime    = 0;  // NOLINT: provided by library

// depth, principal_var, timing arrays are owned by lib/engine usage
extern int depth;           // (declared in header)
extern int times[];         // (declared in header)
extern Move principal_var[];// (declared in header)

// ======= Utilities =======
static void die(const char *fmt, ...) __attribute__((format(printf,1,2)));
static void die(const char *fmt, ...) {
    // Print error then shutdown children and exit.
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "ccheck: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);

    // Best-effort cleanup
    if (g_disp_pid > 0) kill(g_disp_pid, SIGTERM);
    if (g_eng_pid  > 0) kill(g_eng_pid,  SIGTERM);
    // Close pipes/files
    if (g_disp_in)  fclose(g_disp_in);
    if (g_disp_out) fclose(g_disp_out);
    if (g_eng_in)   fclose(g_eng_in);
    if (g_eng_out)  fclose(g_eng_out);
    if (g_tx)       fclose(g_tx);
    // Give children a moment, then SIGKILL
    if (g_disp_pid > 0) { usleep(100*1000); kill(g_disp_pid, SIGKILL); }
    if (g_eng_pid  > 0) { usleep(100*1000); kill(g_eng_pid,  SIGKILL); }

    // Reap to avoid zombies
    while (waitpid(-1, NULL, WNOHANG) > 0) {}

    exit(EXIT_FAILURE);
}

static void info(const char *fmt, ...) __attribute__((format(printf,1,2)));
static void info(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[ccheck] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

// ======= Signal handlers (async-signal-safe) =======
static void on_sigint(int signo)  { (void)signo; g_got_sigint  = 1; }
static void on_sigterm(int signo) { (void)signo; g_got_sigterm = 1; }
static void on_sigpipe(int signo) { (void)signo; g_got_sigpipe = 1; }
static void on_sigchld(int signo) { (void)signo; g_got_sigchld = 1; }

static void install_handlers(void) {
    struct sigaction sa = {0};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sa.sa_handler = on_sigint;  if (sigaction(SIGINT,  &sa, NULL) < 0) die("sigaction SIGINT: %s", strerror(errno));
    sa.sa_handler = on_sigterm; if (sigaction(SIGTERM, &sa, NULL) < 0) die("sigaction SIGTERM: %s", strerror(errno));
    sa.sa_handler = on_sigpipe; if (sigaction(SIGPIPE, &sa, NULL) < 0) die("sigaction SIGPIPE: %s", strerror(errno));
    sa.sa_handler = on_sigchld; if (sigaction(SIGCHLD, &sa, NULL) < 0) die("sigaction SIGCHLD: %s", strerror(errno));
}

static void reap_children_nonblock(void) {
    int saved = errno;
    while (1) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;
        if (pid == g_disp_pid)  g_disp_pid = -1;
        if (pid == g_eng_pid)   g_eng_pid  = -1;
    }
    errno = saved;
}

// ======= Pipe helpers =======
static FILE *fdopen_checked(int fd, const char *mode) {
    FILE *f = fdopen(fd, mode);
    if (!f) die("fdopen: %s", strerror(errno));
    setvbuf(f, NULL, _IOLBF, 0); // line-buffered for deterministic IPC
    return f;
}

static void close_fd_if_open(int *fd) {
    if (*fd >= 0) { close(*fd); *fd = -1; }
}

// Read one line (terminated by \n) from child as a ready/ack or move line.
static bool read_line(FILE *in, char *buf, size_t bufsz) {
    if (!fgets(buf, (int)bufsz, in)) return false; // EOF or error
    return true;
}

// Send exactly one line then SIGHUP to child PID, then read one-line ack.
static bool send_line_hup_expect_ack(FILE *out, pid_t child, FILE *in, const char *line, char *ackbuf, size_t acksz) {
    if (fputs(line, out) == EOF) return false;
    if (fflush(out) == EOF) return false;
    if (kill(child, SIGHUP) < 0) return false;
    if (!read_line(in, ackbuf, acksz)) return false;
    return true;
}

// ======= Child launchers =======
static void spawn_display_if_needed(const Config *cfg) {
    if (cfg->no_display) return;

    int to_disp[2] = {-1,-1};
    int from_disp[2] = {-1,-1};
    if (pipe(to_disp)   < 0) die("pipe to_disp: %s", strerror(errno));
    if (pipe(from_disp) < 0) die("pipe from_disp: %s", strerror(errno));

    pid_t pid = fork();
    if (pid < 0) die("fork xdisp: %s", strerror(errno));

    if (pid == 0) {
        // Child: wire stdin/stdout, close extras, exec xdisp
        if (dup2(to_disp[0], STDIN_FILENO)   < 0) _exit(127);
        if (dup2(from_disp[1], STDOUT_FILENO)< 0) _exit(127);
        close_fd_if_open(&to_disp[0]); close_fd_if_open(&to_disp[1]);
        close_fd_if_open(&from_disp[0]); close_fd_if_open(&from_disp[1]);
        execlp("util/xdisp", "xdisp", NULL);
        _exit(127);
    }

    // Parent
    g_disp_pid = pid;
    close_fd_if_open(&to_disp[0]);
    close_fd_if_open(&from_disp[1]);
    g_disp_in  = fdopen_checked(to_disp[1], "w");   // parent -> child stdin
    g_disp_out = fdopen_checked(from_disp[0], "r"); // parent <- child stdout

    // Wait for single "ready" line from xdisp
    char ready[256];
    if (!read_line(g_disp_out, ready, sizeof(ready))) die("xdisp failed to signal readiness");
    // (Optional) print debug that display is ready
    // info("display ready: %s", ready);
}

// Engine is provided by engine() function in another TU; we only spawn here if -w/-b.
extern void student_engine(Board *);

static void spawn_engine_if_needed(const Config *cfg, Board *bp) {
    if (!cfg->play_white_engine && !cfg->play_black_engine) return; // no engine required

    int to_eng[2] = {-1,-1};
    int from_eng[2] = {-1,-1};
    if (pipe(to_eng)   < 0) die("pipe to_eng: %s", strerror(errno));
    if (pipe(from_eng) < 0) die("pipe from_eng: %s", strerror(errno));

    pid_t pid = fork();
    if (pid < 0) die("fork engine: %s", strerror(errno));

    if (pid == 0) {
        signal(SIGHUP, SIG_IGN);

        // Child: hook up stdin/stdout to pipes and call engine(bp)
        if (dup2(to_eng[0], STDIN_FILENO)    < 0) _exit(127);
        if (dup2(from_eng[1], STDOUT_FILENO) < 0) _exit(127);
        close_fd_if_open(&to_eng[0]); close_fd_if_open(&to_eng[1]);
        close_fd_if_open(&from_eng[0]); close_fd_if_open(&from_eng[1]);
        // In the child, directly call engine()
        student_engine(bp);
        _exit(0);
    }


    // Parent
    g_eng_pid = pid;
    close_fd_if_open(&to_eng[0]);
    close_fd_if_open(&from_eng[1]);
    g_eng_in  = fdopen_checked(to_eng[1], "w");   // parent -> engine stdin
    g_eng_out = fdopen_checked(from_eng[0], "r"); // parent <- engine stdout

    // Some engine implementations may emit a ready line; the spec doesn't require it.
    // We won't block here—parent proceeds immediately.
}

// ======= Argument parsing =======
static void parse_args(Config *cfg, int argc, char **argv) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->avg_time = 0;

    int opt;
    // Leading ':' so getopt returns ':' on missing arg to an option
    while ((opt = getopt(argc, argv, ":wbrvdta:i:o:")) != -1) {
        switch (opt) {
            case 'w': cfg->play_white_engine = true; break;
            case 'b': cfg->play_black_engine = true; break;
            case 'r': cfg->randomized_play   = true; break;
            case 'v': cfg->verbose_stats     = true; break;
            case 'd': cfg->no_display        = true; break;
            case 't': cfg->tournament_mode   = true; break;
            case 'a': cfg->avg_time          = atoi(optarg); break;
            case 'i': cfg->init_file         = optarg; break;
            case 'o': cfg->transcript        = optarg; break;
            case ':': die("missing argument for -%c", optopt);
            default:  die("unknown option -%c", optopt);
        }
    }

    // Set global knobs expected by engine/lib
    randomized = cfg->randomized_play ? 1 : 0;
    verbose   = cfg->verbose_stats   ? 1 : 0;
    avgtime   = cfg->avg_time;
}

// ======= History loading (pushes to display, no engine yet) =======
 static void send_display_move(Board *bp, Player p, Move m) {
    if (g_disp_pid <= 0) return;

    char mvbuf[128];
    FILE *mem = fmemopen(mvbuf, sizeof mvbuf, "w");
    if (!mem) die("fmemopen: %s", strerror(errno));
    print_move(bp, m, mem); fflush(mem); fclose(mem);

    const char *side = (p == X) ? "White" : "Black";  // capitalized
    char line[192];
    snprintf(line, sizeof line, ">%s:%s", side, mvbuf);

    char ack[192];
    if (!send_line_hup_expect_ack(g_disp_in, g_disp_pid, g_disp_out, line, ack, sizeof ack))
        die("display ack failed (history)");
    if (strcmp(ack, "ok\n") != 0)
        die("display ack failed (history)");

 
 }



static void load_history_if_any(Board *bp, const Config *cfg) {
    if (!cfg->init_file) return;
    FILE *f = fopen(cfg->init_file, "r");
    if (!f) die("open -i %s: %s", cfg->init_file, strerror(errno));

    while (1) {
        // read_move_from_pipe reads one move or returns 0 on EOF, validating legality
        Move m = read_move_from_pipe(f, bp);
        if (m == 0) break; // EOF
        Player p = player_to_move(bp); // side about to play
        // Update display BEFORE applying so hops are derived from the correct state
        send_display_move(bp, p, m);
        // Apply on our board state
        apply(bp, m);
        // Minimal transcript of history (optional): you can enhance if needed.
        if (g_tx) {
            int ply = move_number(bp) - 1; // just applied
            int turn = (ply / 2) + 1;
            const char *side = (p == X) ? "white" : "black";
            char mv[128]; FILE *mem = fmemopen(mv, sizeof(mv), "w");
            if (!mem) die("fmemopen: %s", strerror(errno));
            print_move(bp, m, mem); fflush(mem); fclose(mem);
            if (p == X) fprintf(g_tx, "%d. %s:%s", turn, side, mv);
            else        fprintf(g_tx, "%d. ... %s:%s", turn, side, mv);
            fflush(g_tx);
        }
    }
    fclose(f);
}

// ======= Engine notifications & requests =======
static void notify_engine_of_opponent_move(Board *bp, Player mover, const Config *cfg, Move m) {
    if (g_eng_pid <= 0) return;
    bool engine_is_white = cfg->play_white_engine;
    bool engine_is_black = cfg->play_black_engine;
    bool mover_is_opponent = (mover == X && engine_is_black) || (mover == O && engine_is_white);
    if (!mover_is_opponent) return;

    char mvbuf[128]; FILE *mem = fmemopen(mvbuf, sizeof(mvbuf), "w");
    if (!mem) die("fmemopen: %s", strerror(errno));
    print_move(bp, m, mem); fflush(mem); fclose(mem);

    const char *side = (mover == X) ? "white" : "black";
    char line[192]; snprintf(line, sizeof(line), ">%s:%s", side, mvbuf);

    char ack[192];
    if (!send_line_hup_expect_ack(g_eng_in, g_eng_pid, g_eng_out, line, ack, sizeof(ack)))
        die("engine ack failed (forwarding opponent move)");
}

static Move request_move_from_engine(Board *bp) {
    if (g_eng_pid <= 0) die("engine requested but no engine child");
    if (fputs("<\n", g_eng_in) == EOF) die("engine write < failed");
    if (fflush(g_eng_in) == EOF) die("engine flush failed");
    //    if (kill(g_eng_pid, SIGHUP) < 0) die("engine SIGHUP: %s", strerror(errno));
    // Read exactly one move line from engine and validate using the helper

//int c;  //ming
//while ((c = fgetc(g_eng_out)) != EOF) {   // read one character at a time  //ming
//    putchar(c);                    // print it to stdout                       //ming
//}
//fprintf(stderr, "\n"); //ming

fprintf(stderr, "[ccheck] line 361\n"); //ming
    Move m = read_move_from_pipe(g_eng_out, bp);
fprintf(stderr, "[ccheck] line 363\n"); //ming


    if (m == 0) die("engine produced EOF instead of a move");
    return m;

}

static Move request_move_from_display(Board *bp) {
    if (g_disp_pid <= 0) die("display requested but no display child");
    if (fputs("<\n", g_disp_in) == EOF) die("display write < failed");
    if (fflush(g_disp_in) == EOF) die("display flush failed");
    if (kill(g_disp_pid, SIGHUP) < 0) die("display SIGHUP: %s", strerror(errno));


//int c;  //ming
//while ((c = fgetc(g_disp_out)) != EOF) {   // read one character at a time  //ming
//    putchar(c);                    // print it to stdout                       //ming
//}
//fprintf(stderr, "\n"); //ming

fprintf(stderr, "[ccheck] line 384\n"); //ming
    Move m = read_move_from_pipe(g_disp_out, bp);
fprintf(stderr, "[ccheck] line 386\n"); //ming

    if (m == 0) die("display produced EOF instead of a move");
    return m;
}

// ======= Transcript helpers =======
static void write_transcript_move(Board *bp, Player p, Move m) {
    if (!g_tx) return;
    int ply_before_apply = move_number(bp); // pending ply index
    int turn = (ply_before_apply / 2) + 1;
    const char *side = (p == X) ? "white" : "black";
    char mv[128]; FILE *mem = fmemopen(mv, sizeof(mv), "w");
    if (!mem) die("fmemopen: %s", strerror(errno));
    print_move(bp, m, mem); fflush(mem); fclose(mem);
    if (p == X) fprintf(g_tx, "%d. %s:%s", turn, side, mv);
    else        fprintf(g_tx, "%d. ... %s:%s", turn, side, mv);
    fflush(g_tx);
}

// ======= Main game loop (full move flow) =======
static void game_loop(Board *bp, const Config *cfg) {
    info("entering main game loop");

    for (;;) {
        if (g_got_sigint || g_got_sigterm) {
            info("termination requested");
            break;
        }
        if (g_got_sigchld) { reap_children_nonblock(); g_got_sigchld = 0; }
        if (g_got_sigpipe)  die("SIGPIPE encountered (child closed pipe)");

        // Check game end
        int ended = game_over(bp);
        if (ended != 0) {
            if (ended == 1) fprintf(stdout, "X (white) wins!");
            else            fprintf(stdout, "O (black) wins!");
            fflush(stdout);
            break; // then fall into shutdown
        }

        Player p = player_to_move(bp);
        bool p_is_engine = (p == X) ? cfg->play_white_engine : cfg->play_black_engine;

        // Acquire move according to mode
        Move m = 0;
        bool came_from_display = false;

        if (p_is_engine) {

            m = request_move_from_engine(bp);
        } else if (!cfg->no_display && !cfg->tournament_mode) {  //not engine play
            m = request_move_from_display(bp);
            came_from_display = true;           // <-- mark source
        } else {
            m = read_move_interactive(bp);      // stdin ASCII
        }

fprintf(stderr, "[ccheck] line 444\n"); //ming
        if (m == 0) {
            info("EOF/zero move received; exiting");
            break;
        }



        // Timekeeping: charge the mover for time up to this point before applying
        setclock(p);

        // Before applying, update transcript based on current board and mover
        write_transcript_move(bp, p, m);

fprintf(stderr, "[ccheck] reached echo-to-display gate. no_display=%d, came_from_display=%d\n", (int)cfg->no_display, (int)came_from_display); //ming

        // Only echo to display if it didn't originate from the display
        if (!cfg->no_display && !came_from_display) {
            /* Format the move exactly once using the library helper */
            char mv[128];
            FILE *mem = fmemopen(mv, sizeof mv, "w");
            if (!mem) die("fmemopen: %s", strerror(errno));
            print_move(bp, m, mem); fflush(mem); fclose(mem);  /* mv now ends with '\n' */

            /* Prefix with the side, as xdisp expects */
            const char *side = (p == X) ? "white" : "black";
fprintf(stderr, "[ccheck -> xdisp] >%s:%s", side, mv); //ming

            if (fprintf(g_disp_in, ">%s:%s", side, mv) < 0 || fflush(g_disp_in) == EOF) {
                die("write to display failed");
            }

            /* Nudge the display so it processes the forwarded move immediately */
            if (kill(g_disp_pid, SIGHUP) < 0) {
                die("display SIGHUP: %s", strerror(errno));
            }

            /* Wait for the display's ack to ensure it processed the move */
            char ack[16];
            if (!fgets(ack, sizeof ack, g_disp_out) || strcmp(ack, "ok\n") != 0) {
                die("display ack failed");
            }
fprintf(stderr, "[xdisp -> ccheck] %s", ack);//ming
        }

        // If in tournament mode and the mover was the ENGINE, print @@@-prefixed line
        if (cfg->tournament_mode && p_is_engine) {
            const char *side = (p == X) ? "white" : "black";
            char mv[128]; FILE *mem = fmemopen(mv, sizeof(mv), "w");
            if (!mem) die("fmemopen: %s", strerror(errno));
            print_move(bp, m, mem); fflush(mem); fclose(mem);
            fprintf(stdout, "@@@%s:%s", side, mv);
            fflush(stdout);
        }
fprintf(stderr, "[ccheck] line 499\n"); //ming
        // Apply the move to our authoritative board state
        apply(bp, m);

        // If the engine exists and this move was by its opponent, notify the engine
        notify_engine_of_opponent_move(bp, p, cfg, m);
    }
}

// ======= Public entry point =======
int ccheck(int argc, char *argv[]) {
    Config cfg; parse_args(&cfg, argc, argv);

    install_handlers();

    // Open transcript if requested
    if (cfg.transcript) {
        g_tx = fopen(cfg.transcript, "w");
        if (!g_tx) die("open -o %s: %s", cfg.transcript, strerror(errno));
    }

    // Create initial board
    Board *bp = newbd();
    if (!bp) die("newbd failed");

    // Spawn display (unless -d)
    spawn_display_if_needed(&cfg);

    // Load initial history (-i), updating display as we go
    load_history_if_any(bp, &cfg);

    // Spawn engine process if needed (after history applied)
    spawn_engine_if_needed(&cfg, bp);

    // Enter main game loop (stub)
    game_loop(bp, &cfg);

    // Graceful shutdown path — close fds, kill children (if any), reap
    if (g_disp_pid > 0) kill(g_disp_pid, SIGTERM);
    if (g_eng_pid  > 0) kill(g_eng_pid,  SIGTERM);

    if (g_disp_in)  { fclose(g_disp_in);  g_disp_in = NULL; }
    if (g_disp_out) { fclose(g_disp_out); g_disp_out = NULL; }
    if (g_eng_in)   { fclose(g_eng_in);   g_eng_in = NULL; }
    if (g_eng_out)  { fclose(g_eng_out);  g_eng_out = NULL; }
    if (g_tx)       { fclose(g_tx);       g_tx = NULL; }

    // Reap children
    g_got_sigchld = 1; // force a reap attempt
    if (g_got_sigchld) reap_children_nonblock();

    return EXIT_SUCCESS;
}

